/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xi_evidence.c - Revision and invalidation contracts for local proofs
 */

#include "../test_framework.h"

#include "ir/xi.h"
#include "ir/xi_analysis.h"
#include "ir/xi_analysis_manager.h"
#include "ir/xi_evidence.h"
#include "ir/xi_range.h"
#include "runtime/value/xtype.h"
#include <assert.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};

static XiFunc *make_func(void) {
    XiFunc *func = xi_func_new("evidence", &stub_int);
    assert(func != NULL);
    XiBlock *entry = xi_block_new(func);
    assert(entry != NULL);
    XiValue *value = xi_const_int(func, entry, 1, &stub_int);
    assert(value != NULL);
    xi_block_set_return(entry, value);
    return func;
}

static const XiEvidenceRecord *publish_function(XiFunc *func, XiEvidenceDomain domain,
                                                XiProofState state, XiEvidenceReason reason) {
    return xi_evidence_publish(func, domain, xi_evidence_subject_function(), state, reason,
                               XI_EVIDENCE_PRODUCER_TEST, 0, NULL);
}

TEST(published_proof_is_bound_to_current_revision) {
    XiFunc *func = make_func();
    const XiEvidenceRecord *record =
        publish_function(func, XI_EVD_RANGE, XI_PROOF_PROVEN, XI_EVIDENCE_REASON_NONE);
    ASSERT_NOT_NULL(record);
    ASSERT_TRUE(xi_evidence_domain_is_current(func, XI_EVD_RANGE));
    ASSERT_TRUE(xi_evidence_find_by_id(func, record->id) == record);
    ASSERT_EQ_UINT(record->stamp.ir_revision, func->ir_revision);
    ASSERT_EQ_UINT(record->stamp.cfg_revision, func->cfg_version);
    xi_func_free(func);
}

TEST(rewrite_invalidates_declared_domains) {
    XiFunc *func = make_func();
    publish_function(func, XI_EVD_RANGE, XI_PROOF_PROVEN, XI_EVIDENCE_REASON_NONE);
    publish_function(func, XI_EVD_ESCAPE, XI_PROOF_PROVEN, XI_EVIDENCE_REASON_NONE);

    uint64_t old_revision = func->ir_revision;
    xi_evidence_note_rewrite(func, false, true, false, XI_EVD_RANGE | XI_EVD_ESCAPE);

    ASSERT_EQ_UINT(func->ir_revision, old_revision + 1);
    ASSERT_FALSE(xi_evidence_domain_is_current(func, XI_EVD_RANGE));
    ASSERT_FALSE(xi_evidence_domain_is_current(func, XI_EVD_ESCAPE));
    ASSERT_EQ_INT(xi_evidence_query(func, XI_EVD_RANGE, xi_evidence_subject_function()).reason,
                  XI_EVIDENCE_REASON_INVALIDATED_BY_REWRITE);
    xi_func_free(func);
}

TEST(stale_stamp_fails_closed_without_explicit_invalidation) {
    XiFunc *func = make_func();
    publish_function(func, XI_EVD_EFFECT, XI_PROOF_PROVEN, XI_EVIDENCE_REASON_NONE);
    func->ir_revision++;

    XiEvidenceView view = xi_evidence_query(func, XI_EVD_EFFECT, xi_evidence_subject_function());
    ASSERT_FALSE(view.current);
    ASSERT_EQ_INT(view.reason, XI_EVIDENCE_REASON_IR_REVISION_MISMATCH);
    xi_func_free(func);
}

TEST(cfg_revision_mismatch_rejects_old_proof) {
    XiFunc *func = make_func();
    publish_function(func, XI_EVD_ALIAS, XI_PROOF_PROVEN, XI_EVIDENCE_REASON_NONE);
    xi_cfg_invalidate(func);

    XiEvidenceView view = xi_evidence_query(func, XI_EVD_ALIAS, xi_evidence_subject_function());
    ASSERT_FALSE(view.current);
    ASSERT_EQ_INT(view.reason, XI_EVIDENCE_REASON_CFG_REVISION_MISMATCH);
    xi_func_free(func);
}

TEST(unproven_is_a_current_formal_result) {
    XiFunc *func = make_func();
    const XiEvidenceRecord *record = publish_function(func, XI_EVD_NOALLOC, XI_PROOF_UNPROVEN,
                                                      XI_EVIDENCE_REASON_INCOMPLETE_GLOBAL_FACTS);
    ASSERT_NOT_NULL(record);
    ASSERT_EQ_INT(record->state, XI_PROOF_UNPROVEN);
    ASSERT_TRUE(xi_evidence_domain_is_current(func, XI_EVD_NOALLOC));
    ASSERT_FALSE(xi_evidence_domain_is_proven_current(func, XI_EVD_NOALLOC));
    xi_func_free(func);
}

TEST(analysis_manager_rejects_current_unproven_result) {
    XiFunc *func = make_func();
    XiAnalysisManager manager;
    xi_analysis_manager_init(&manager, func);
    char error[256] = {0};
    publish_function(func, XI_EVD_CALL_TARGET, XI_PROOF_UNPROVEN,
                     XI_EVIDENCE_REASON_INCOMPLETE_GLOBAL_FACTS);
    ASSERT_FALSE(
        xi_analysis_require_proven_domains(&manager, XI_EVD_CALL_TARGET, error, sizeof(error)));
    ASSERT_TRUE(strstr(error, "call_target") != NULL);
    xi_func_free(func);
}

TEST(analysis_manager_recomputes_supported_evidence) {
    XiFunc *func = make_func();
    XiAnalysisManager manager;
    xi_analysis_manager_init(&manager, func);
    char error[256];
    ASSERT_TRUE(xi_analysis_require_proven_domains(&manager, XI_EVD_RANGE | XI_EVD_ALIAS, error,
                                                   sizeof(error)));
    ASSERT_TRUE(xi_evidence_domain_is_current(func, XI_EVD_RANGE));
    ASSERT_TRUE(xi_evidence_domain_is_current(func, XI_EVD_ALIAS));

    xi_evidence_note_rewrite(func, false, true, false, XI_EVD_RANGE | XI_EVD_ALIAS);
    ASSERT_FALSE(xi_evidence_domain_is_current(func, XI_EVD_RANGE));
    ASSERT_TRUE(xi_analysis_require_proven_domains(&manager, XI_EVD_RANGE | XI_EVD_ALIAS, error,
                                                   sizeof(error)));
    xi_func_free(func);
}

TEST(analysis_manager_proves_empty_call_target_set) {
    XiFunc *func = make_func();
    XiAnalysisManager manager;
    xi_analysis_manager_init(&manager, func);
    char error[256] = {0};
    ASSERT_TRUE(
        xi_analysis_require_proven_domains(&manager, XI_EVD_CALL_TARGET, error, sizeof(error)));
    ASSERT_TRUE(xi_evidence_domain_is_proven_current(func, XI_EVD_CALL_TARGET));
    xi_func_free(func);
}

TEST(analysis_manager_routes_every_local_domain) {
    XiFunc *func = make_func();
    func->allocation_effect_complete = true;
    func->allocation_state = 0;
    XiAnalysisManager manager;
    xi_analysis_manager_init(&manager, func);
    char error[256] = {0};
    ASSERT_TRUE(xi_analysis_require_proven_domains(&manager, XI_EVD_ALL, error, sizeof(error)));
    for (uint32_t bit = 1; bit <= XI_EVD_MEMSSA; bit <<= 1u)
        ASSERT_TRUE(xi_evidence_domain_is_proven_current(func, (XiEvidenceDomain) bit));
    xi_func_free(func);
}

TEST(value_proof_cannot_be_consumed_for_another_value) {
    XiFunc *func = make_func();
    XiValue *first = func->entry->values[0];
    XiValue *second = xi_const_int(func, func->entry, 2, &stub_int);
    XiEvidencePayload payload = {
        .kind = XI_EVIDENCE_PAYLOAD_RANGE,
        .as.range = {.lo = 1, .hi = 1, .is_top = false, .is_bot = false},
    };
    ASSERT_NOT_NULL(xi_evidence_publish(func, XI_EVD_RANGE, xi_evidence_subject_value(first),
                                        XI_PROOF_PROVEN, XI_EVIDENCE_REASON_NONE,
                                        XI_EVIDENCE_PRODUCER_TEST, first->line, &payload));
    ASSERT_TRUE(
        xi_evidence_is_proven_current(func, XI_EVD_RANGE, xi_evidence_subject_value(first)));
    XiEvidenceView wrong = xi_evidence_query(func, XI_EVD_RANGE, xi_evidence_subject_value(second));
    ASSERT_FALSE(wrong.current);
    ASSERT_EQ_INT(wrong.reason, XI_EVIDENCE_REASON_SUBJECT_NOT_FOUND);
    xi_func_free(func);
}

TEST(orphan_value_proof_is_pruned_after_rewrite) {
    XiFunc *func = make_func();
    XiValue *dead = xi_const_int(func, func->entry, 99, &stub_int);
    XiEvidenceSubject subject = xi_evidence_subject_value(dead);
    ASSERT_NOT_NULL(xi_evidence_publish(func, XI_EVD_RANGE, subject, XI_PROOF_PROVEN,
                                        XI_EVIDENCE_REASON_NONE, XI_EVIDENCE_PRODUCER_TEST,
                                        dead->line, NULL));
    ASSERT_TRUE(xi_evidence_is_current(func, XI_EVD_RANGE, subject));

    ASSERT_TRUE(func->entry->nvalues > 0);
    func->entry->nvalues--;
    xi_evidence_note_rewrite(func, false, true, false, XI_EVD_RANGE);
    XiEvidenceView view = xi_evidence_query(func, XI_EVD_RANGE, subject);
    ASSERT_FALSE(view.current);
    ASSERT_EQ_INT(view.reason, XI_EVIDENCE_REASON_SUBJECT_NOT_FOUND);
    xi_func_free(func);
}

TEST(range_query_rejects_stale_subject_payload) {
    XiFunc *func = make_func();
    XiValue *left = xi_const_int(func, func->entry, 20, &stub_int);
    XiValue *right = xi_const_int(func, func->entry, 22, &stub_int);
    XiValue *value = xi_value_new(func, func->entry, XI_ADD, &stub_int, 2);
    value->args[0] = left;
    value->args[1] = right;
    xi_range_analyze(func);
    ASSERT_TRUE(xi_range_is_const(xi_range_of(value)));

    func->ir_revision++;
    ASSERT_TRUE(xi_range_of(value).is_top);
    xi_func_free(func);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("revisioned Xi evidence");
RUN_TEST(published_proof_is_bound_to_current_revision);
RUN_TEST(rewrite_invalidates_declared_domains);
RUN_TEST(stale_stamp_fails_closed_without_explicit_invalidation);
RUN_TEST(cfg_revision_mismatch_rejects_old_proof);
RUN_TEST(unproven_is_a_current_formal_result);
RUN_TEST(analysis_manager_rejects_current_unproven_result);
RUN_TEST(analysis_manager_recomputes_supported_evidence);
RUN_TEST(analysis_manager_proves_empty_call_target_set);
RUN_TEST(analysis_manager_routes_every_local_domain);
RUN_TEST(value_proof_cannot_be_consumed_for_another_value);
RUN_TEST(orphan_value_proof_is_pruned_after_rewrite);
RUN_TEST(range_query_rejects_stale_subject_payload);
TEST_MAIN_END()
