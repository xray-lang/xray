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

TEST(published_proof_is_bound_to_current_revision) {
    XiFunc *func = make_func();
    const XiEvidenceRecord *record =
        xi_evidence_publish(func, XI_EVD_RANGE, XI_PROOF_PROVEN, XI_EVIDENCE_REASON_NONE, "test");
    ASSERT_NOT_NULL(record);
    ASSERT_TRUE(xi_evidence_is_current(func, XI_EVD_RANGE));
    ASSERT_EQ_UINT(record->stamp.ir_revision, func->ir_revision);
    ASSERT_EQ_UINT(record->stamp.cfg_revision, func->cfg_version);
    xi_func_free(func);
}

TEST(rewrite_invalidates_declared_domains) {
    XiFunc *func = make_func();
    xi_evidence_publish(func, XI_EVD_RANGE, XI_PROOF_PROVEN, XI_EVIDENCE_REASON_NONE, "range");
    xi_evidence_publish(func, XI_EVD_ESCAPE, XI_PROOF_PROVEN, XI_EVIDENCE_REASON_NONE, "escape");

    uint64_t old_revision = func->ir_revision;
    xi_evidence_note_rewrite(func, false, true, false, XI_EVD_RANGE | XI_EVD_ESCAPE);

    ASSERT_EQ_UINT(func->ir_revision, old_revision + 1);
    ASSERT_FALSE(xi_evidence_is_current(func, XI_EVD_RANGE));
    ASSERT_FALSE(xi_evidence_is_current(func, XI_EVD_ESCAPE));
    ASSERT_EQ_INT(xi_evidence_query(func, XI_EVD_RANGE).reason,
                  XI_EVIDENCE_REASON_INVALIDATED_BY_REWRITE);
    xi_func_free(func);
}

TEST(stale_stamp_fails_closed_without_explicit_invalidation) {
    XiFunc *func = make_func();
    xi_evidence_publish(func, XI_EVD_EFFECT, XI_PROOF_PROVEN, XI_EVIDENCE_REASON_NONE, "effect");
    func->ir_revision++;

    XiEvidenceView view = xi_evidence_query(func, XI_EVD_EFFECT);
    ASSERT_FALSE(view.current);
    ASSERT_EQ_INT(view.reason, XI_EVIDENCE_REASON_IR_REVISION_MISMATCH);
    xi_func_free(func);
}

TEST(cfg_revision_mismatch_rejects_old_proof) {
    XiFunc *func = make_func();
    xi_evidence_publish(func, XI_EVD_ALIAS, XI_PROOF_PROVEN, XI_EVIDENCE_REASON_NONE, "alias");
    xi_cfg_invalidate(func);

    XiEvidenceView view = xi_evidence_query(func, XI_EVD_ALIAS);
    ASSERT_FALSE(view.current);
    ASSERT_EQ_INT(view.reason, XI_EVIDENCE_REASON_CFG_REVISION_MISMATCH);
    xi_func_free(func);
}

TEST(unproven_is_a_current_formal_result) {
    XiFunc *func = make_func();
    const XiEvidenceRecord *record =
        xi_evidence_publish(func, XI_EVD_NOALLOC, XI_PROOF_UNPROVEN,
                            XI_EVIDENCE_REASON_INCOMPLETE_GLOBAL_FACTS, "noalloc");
    ASSERT_NOT_NULL(record);
    ASSERT_EQ_INT(record->state, XI_PROOF_UNPROVEN);
    ASSERT_TRUE(xi_evidence_is_current(func, XI_EVD_NOALLOC));
    ASSERT_FALSE(xi_evidence_is_proven_current(func, XI_EVD_NOALLOC));
    xi_func_free(func);
}

TEST(analysis_manager_rejects_current_unproven_result) {
    XiFunc *func = make_func();
    char error[256] = {0};
    xi_evidence_publish(func, XI_EVD_CALL_TARGET, XI_PROOF_UNPROVEN,
                        XI_EVIDENCE_REASON_INCOMPLETE_GLOBAL_FACTS, "call-target");
    ASSERT_FALSE(xi_analysis_require(func, XI_EVD_CALL_TARGET, error, sizeof(error)));
    ASSERT_TRUE(strstr(error, "call_target") != NULL);
    xi_func_free(func);
}

TEST(analysis_manager_recomputes_supported_evidence) {
    XiFunc *func = make_func();
    char error[256];
    ASSERT_TRUE(xi_analysis_require(func, XI_EVD_RANGE | XI_EVD_ALIAS, error, sizeof(error)));
    ASSERT_TRUE(xi_evidence_is_current(func, XI_EVD_RANGE));
    ASSERT_TRUE(xi_evidence_is_current(func, XI_EVD_ALIAS));

    xi_evidence_note_rewrite(func, false, true, false, XI_EVD_RANGE | XI_EVD_ALIAS);
    ASSERT_FALSE(xi_evidence_is_current(func, XI_EVD_RANGE));
    ASSERT_TRUE(xi_analysis_require(func, XI_EVD_RANGE | XI_EVD_ALIAS, error, sizeof(error)));
    xi_func_free(func);
}

TEST(analysis_manager_rejects_domain_without_producer) {
    XiFunc *func = make_func();
    char error[256] = {0};
    ASSERT_FALSE(xi_analysis_require(func, XI_EVD_CALL_TARGET, error, sizeof(error)));
    ASSERT_TRUE(strstr(error, "call_target") != NULL);
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
RUN_TEST(analysis_manager_rejects_domain_without_producer);
TEST_MAIN_END()
