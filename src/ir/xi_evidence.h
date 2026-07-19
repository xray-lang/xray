/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_evidence.h - Revision-bound local proof store for Xi IR
 */

#ifndef XI_EVIDENCE_H
#define XI_EVIDENCE_H

#include "xi.h"
#include "../base/xdefs.h"
#include <stdbool.h>
#include <stdint.h>

typedef uint32_t XiEvidenceDomainMask;

typedef enum XiEvidenceDomain {
    XI_EVD_RANGE = 1u << 0,
    XI_EVD_ESCAPE = 1u << 1,
    XI_EVD_ALIAS = 1u << 2,
    XI_EVD_PROVENANCE = 1u << 3,
    XI_EVD_EFFECT = 1u << 4,
    XI_EVD_OWNERSHIP = 1u << 5,
    XI_EVD_LIFETIME = 1u << 6,
    XI_EVD_NOALLOC = 1u << 7,
    XI_EVD_CALL_TARGET = 1u << 8,
    XI_EVD_MEMSSA = 1u << 9,
} XiEvidenceDomain;

#define XI_EVD_ALL ((XiEvidenceDomainMask) ((1u << 10) - 1u))

typedef enum XiProofState {
    XI_PROOF_INVALID = 0,
    XI_PROOF_PROVEN,
    XI_PROOF_UNPROVEN,
} XiProofState;

typedef enum XiEvidenceReason {
    XI_EVIDENCE_REASON_NONE = 0,
    XI_EVIDENCE_REASON_NOT_ANALYZED,
    XI_EVIDENCE_REASON_IR_REVISION_MISMATCH,
    XI_EVIDENCE_REASON_CFG_REVISION_MISMATCH,
    XI_EVIDENCE_REASON_MEMORY_REVISION_MISMATCH,
    XI_EVIDENCE_REASON_CALL_REVISION_MISMATCH,
    XI_EVIDENCE_REASON_INVALIDATED_BY_REWRITE,
    XI_EVIDENCE_REASON_UNSUPPORTED_SUBJECT,
    XI_EVIDENCE_REASON_SUBJECT_NOT_FOUND,
    XI_EVIDENCE_REASON_SUBJECT_DELETED,
    XI_EVIDENCE_REASON_PRODUCER_UNAVAILABLE,
    XI_EVIDENCE_REASON_INCOMPLETE_GLOBAL_FACTS,
} XiEvidenceReason;

typedef uint64_t XiEvidenceId;

typedef enum XiEvidenceSubjectKind {
    XI_EVIDENCE_SUBJECT_INVALID = 0,
    XI_EVIDENCE_SUBJECT_FUNCTION,
    XI_EVIDENCE_SUBJECT_VALUE,
    XI_EVIDENCE_SUBJECT_CALLSITE,
} XiEvidenceSubjectKind;

/* A subject ID is stable for the lifetime of its owning Xi function. */
typedef struct XiEvidenceSubject {
    XiEvidenceSubjectKind kind;
    uint64_t id;
} XiEvidenceSubject;

typedef enum XiEvidenceProducer {
    XI_EVIDENCE_PRODUCER_INVALID = 0,
    XI_EVIDENCE_PRODUCER_RANGE_ANALYSIS,
    XI_EVIDENCE_PRODUCER_ESCAPE_ANALYSIS,
    XI_EVIDENCE_PRODUCER_TBAA,
    XI_EVIDENCE_PRODUCER_MEMSSA,
    XI_EVIDENCE_PRODUCER_EFFECT_SCAN,
    XI_EVIDENCE_PRODUCER_OWNERSHIP_ANALYSIS,
    XI_EVIDENCE_PRODUCER_LIFETIME_ANALYSIS,
    XI_EVIDENCE_PRODUCER_ALLOCATION_PUBLICATION,
    XI_EVIDENCE_PRODUCER_CALL_TARGET_PUBLICATION,
    XI_EVIDENCE_PRODUCER_PROVENANCE_ANALYSIS,
    XI_EVIDENCE_PRODUCER_TEST,
} XiEvidenceProducer;

typedef enum XiEvidencePayloadKind {
    XI_EVIDENCE_PAYLOAD_NONE = 0,
    XI_EVIDENCE_PAYLOAD_RANGE,
    XI_EVIDENCE_PAYLOAD_U64_PAIR,
} XiEvidencePayloadKind;

typedef struct XiEvidenceRangePayload {
    int64_t lo;
    int64_t hi;
    bool is_top;
    bool is_bot;
} XiEvidenceRangePayload;

typedef struct XiEvidencePayload {
    XiEvidencePayloadKind kind;
    union {
        XiEvidenceRangePayload range;
        struct {
            uint64_t first;
            uint64_t second;
        } u64_pair;
    } as;
} XiEvidencePayload;

typedef struct XiEvidenceStamp {
    uint64_t ir_revision;
    uint64_t cfg_revision;
    uint64_t memory_revision;
    uint64_t call_revision;
} XiEvidenceStamp;

typedef struct XiEvidenceRecord {
    XiEvidenceId id;
    XiEvidenceDomain domain;
    XiEvidenceSubject subject;
    XiProofState state;
    XiEvidenceReason reason;
    XiEvidenceProducer producer;
    const char *source_file;
    uint32_t source_line;
    XiEvidencePayload payload;
    XiEvidenceStamp stamp;
} XiEvidenceRecord;

typedef struct XiEvidenceView {
    const XiEvidenceRecord *record;
    bool current;
    XiEvidenceReason reason;
} XiEvidenceView;

XR_FUNC void xi_evidence_init_func(XiFunc *func);
XR_FUNC void xi_evidence_dispose_func(XiFunc *func);
XR_FUNC XiEvidenceStamp xi_evidence_current_stamp(const XiFunc *func);
XR_FUNC XiEvidenceSubject xi_evidence_subject_function(void);
XR_FUNC XiEvidenceSubject xi_evidence_subject_value(const XiValue *value);
XR_FUNC XiEvidenceSubject xi_evidence_subject_callsite(const XiValue *call);
XR_FUNC const XiEvidenceRecord *
xi_evidence_publish(XiFunc *func, XiEvidenceDomain domain, XiEvidenceSubject subject,
                    XiProofState state, XiEvidenceReason reason, XiEvidenceProducer producer,
                    uint32_t source_line, const XiEvidencePayload *payload);
XR_FUNC XiEvidenceView xi_evidence_query(const XiFunc *func, XiEvidenceDomain domain,
                                         XiEvidenceSubject subject);
XR_FUNC const XiEvidenceRecord *xi_evidence_find_by_id(const XiFunc *func, XiEvidenceId id);
XR_FUNC bool xi_evidence_is_current(const XiFunc *func, XiEvidenceDomain domain,
                                    XiEvidenceSubject subject);
XR_FUNC bool xi_evidence_is_proven_current(const XiFunc *func, XiEvidenceDomain domain,
                                           XiEvidenceSubject subject);
XR_FUNC bool xi_evidence_domain_is_current(const XiFunc *func, XiEvidenceDomain domain);
XR_FUNC bool xi_evidence_domain_is_proven_current(const XiFunc *func, XiEvidenceDomain domain);
XR_FUNC void xi_evidence_invalidate(XiFunc *func, XiEvidenceDomainMask domains,
                                    XiEvidenceReason reason);
XR_FUNC void xi_evidence_prune_orphans(XiFunc *func);
XR_FUNC void xi_evidence_note_rewrite(XiFunc *func, bool cfg_changed, bool values_changed,
                                      bool types_changed, XiEvidenceDomainMask invalidates);
XR_FUNC const char *xi_evidence_domain_name(XiEvidenceDomain domain);
XR_FUNC const char *xi_evidence_reason_name(XiEvidenceReason reason);
XR_FUNC const char *xi_evidence_subject_kind_name(XiEvidenceSubjectKind kind);
XR_FUNC const char *xi_evidence_producer_name(XiEvidenceProducer producer);
XR_FUNC void xi_evidence_dump(const XiFunc *func, void *file);

#endif  // XI_EVIDENCE_H
