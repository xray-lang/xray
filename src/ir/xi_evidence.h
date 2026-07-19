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
    XI_EVIDENCE_REASON_INCOMPLETE_GLOBAL_FACTS,
} XiEvidenceReason;

typedef struct XiEvidenceStamp {
    uint64_t ir_revision;
    uint64_t cfg_revision;
    uint64_t memory_revision;
    uint64_t call_revision;
} XiEvidenceStamp;

typedef struct XiEvidenceRecord {
    uint64_t id;
    XiEvidenceDomain domain;
    XiProofState state;
    XiEvidenceReason reason;
    const char *producer;
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
XR_FUNC const XiEvidenceRecord *xi_evidence_publish(XiFunc *func, XiEvidenceDomain domain,
                                                    XiProofState state, XiEvidenceReason reason,
                                                    const char *producer);
XR_FUNC XiEvidenceView xi_evidence_query(const XiFunc *func, XiEvidenceDomain domain);
XR_FUNC bool xi_evidence_is_current(const XiFunc *func, XiEvidenceDomain domain);
XR_FUNC bool xi_evidence_is_proven_current(const XiFunc *func, XiEvidenceDomain domain);
XR_FUNC void xi_evidence_invalidate(XiFunc *func, XiEvidenceDomainMask domains,
                                    XiEvidenceReason reason);
XR_FUNC void xi_evidence_note_rewrite(XiFunc *func, bool cfg_changed, bool values_changed,
                                      bool types_changed, XiEvidenceDomainMask invalidates);
XR_FUNC const char *xi_evidence_domain_name(XiEvidenceDomain domain);
XR_FUNC const char *xi_evidence_reason_name(XiEvidenceReason reason);
XR_FUNC void xi_evidence_dump(const XiFunc *func, void *file);

#endif  // XI_EVIDENCE_H
