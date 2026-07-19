/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#include "xi_evidence.h"
#include "../base/xmalloc.h"
#include <stdio.h>
#include <string.h>

#define XI_EVIDENCE_DOMAIN_COUNT 10u

struct XiEvidenceSet {
    XiEvidenceRecord records[XI_EVIDENCE_DOMAIN_COUNT];
    uint64_t next_id;
};

static int domain_index(XiEvidenceDomain domain) {
    uint32_t value = (uint32_t) domain;
    if (value == 0 || (value & (value - 1u)) != 0 || value > XI_EVD_MEMSSA)
        return -1;
    int index = 0;
    while ((value >>= 1u) != 0)
        index++;
    return index;
}

static XiEvidenceReason stale_reason(const XiFunc *func, const XiEvidenceRecord *record) {
    if (!func || !record || record->state == XI_PROOF_INVALID)
        return record && record->reason != XI_EVIDENCE_REASON_NONE
                   ? record->reason
                   : XI_EVIDENCE_REASON_NOT_ANALYZED;
    if (record->stamp.ir_revision != func->ir_revision)
        return XI_EVIDENCE_REASON_IR_REVISION_MISMATCH;
    if (record->stamp.cfg_revision != func->cfg_version)
        return XI_EVIDENCE_REASON_CFG_REVISION_MISMATCH;
    if (record->stamp.memory_revision != func->memory_revision)
        return XI_EVIDENCE_REASON_MEMORY_REVISION_MISMATCH;
    if (record->stamp.call_revision != func->call_revision)
        return XI_EVIDENCE_REASON_CALL_REVISION_MISMATCH;
    return XI_EVIDENCE_REASON_NONE;
}

void xi_evidence_init_func(XiFunc *func) {
    if (!func)
        return;
    func->ir_revision = 1;
    func->memory_revision = 1;
    func->call_revision = 1;
    func->evidence = (struct XiEvidenceSet *) xr_calloc(1, sizeof(struct XiEvidenceSet));
    if (func->evidence)
        func->evidence->next_id = 1;
}

void xi_evidence_dispose_func(XiFunc *func) {
    if (!func)
        return;
    xr_free(func->evidence);
    func->evidence = NULL;
}

XiEvidenceStamp xi_evidence_current_stamp(const XiFunc *func) {
    XiEvidenceStamp stamp = {0};
    if (!func)
        return stamp;
    stamp.ir_revision = func->ir_revision;
    stamp.cfg_revision = func->cfg_version;
    stamp.memory_revision = func->memory_revision;
    stamp.call_revision = func->call_revision;
    return stamp;
}

const XiEvidenceRecord *xi_evidence_publish(XiFunc *func, XiEvidenceDomain domain,
                                            XiProofState state, XiEvidenceReason reason,
                                            const char *producer) {
    int index = domain_index(domain);
    if (!func || !func->evidence || index < 0 || state == XI_PROOF_INVALID)
        return NULL;
    XiEvidenceRecord *record = &func->evidence->records[index];
    memset(record, 0, sizeof(*record));
    record->id = func->evidence->next_id++;
    record->domain = domain;
    record->state = state;
    record->reason = reason;
    record->producer = producer;
    record->stamp = xi_evidence_current_stamp(func);
    return record;
}

XiEvidenceView xi_evidence_query(const XiFunc *func, XiEvidenceDomain domain) {
    XiEvidenceView view = {0};
    int index = domain_index(domain);
    if (!func || !func->evidence || index < 0) {
        view.reason = XI_EVIDENCE_REASON_NOT_ANALYZED;
        return view;
    }
    view.record = &func->evidence->records[index];
    view.reason = stale_reason(func, view.record);
    view.current = view.record->id != 0 && view.reason == XI_EVIDENCE_REASON_NONE;
    return view;
}

bool xi_evidence_is_current(const XiFunc *func, XiEvidenceDomain domain) {
    return xi_evidence_query(func, domain).current;
}

bool xi_evidence_is_proven_current(const XiFunc *func, XiEvidenceDomain domain) {
    XiEvidenceView view = xi_evidence_query(func, domain);
    return view.current && view.record && view.record->state == XI_PROOF_PROVEN;
}

void xi_evidence_invalidate(XiFunc *func, XiEvidenceDomainMask domains, XiEvidenceReason reason) {
    if (!func || !func->evidence)
        return;
    for (uint32_t i = 0; i < XI_EVIDENCE_DOMAIN_COUNT; i++) {
        XiEvidenceRecord *record = &func->evidence->records[i];
        if (((uint32_t) record->domain & domains) == 0)
            continue;
        record->state = XI_PROOF_INVALID;
        record->reason = reason;
    }
}

void xi_evidence_note_rewrite(XiFunc *func, bool cfg_changed, bool values_changed,
                              bool types_changed, XiEvidenceDomainMask invalidates) {
    if (!func || (!cfg_changed && !values_changed && !types_changed))
        return;
    func->ir_revision++;
    if (func->ir_revision == 0)
        func->ir_revision = 1;
    if (invalidates & (XI_EVD_ALIAS | XI_EVD_PROVENANCE | XI_EVD_MEMSSA)) {
        func->memory_revision++;
        if (func->memory_revision == 0)
            func->memory_revision = 1;
    }
    if (invalidates & (XI_EVD_EFFECT | XI_EVD_ESCAPE | XI_EVD_NOALLOC | XI_EVD_CALL_TARGET)) {
        func->call_revision++;
        if (func->call_revision == 0)
            func->call_revision = 1;
    }
    xi_evidence_invalidate(func, invalidates, XI_EVIDENCE_REASON_INVALIDATED_BY_REWRITE);
}

const char *xi_evidence_domain_name(XiEvidenceDomain domain) {
    switch (domain) {
        case XI_EVD_RANGE:
            return "range";
        case XI_EVD_ESCAPE:
            return "escape";
        case XI_EVD_ALIAS:
            return "alias";
        case XI_EVD_PROVENANCE:
            return "provenance";
        case XI_EVD_EFFECT:
            return "effect";
        case XI_EVD_OWNERSHIP:
            return "ownership";
        case XI_EVD_LIFETIME:
            return "lifetime";
        case XI_EVD_NOALLOC:
            return "noalloc";
        case XI_EVD_CALL_TARGET:
            return "call_target";
        case XI_EVD_MEMSSA:
            return "memssa";
    }
    return "invalid";
}

const char *xi_evidence_reason_name(XiEvidenceReason reason) {
    switch (reason) {
        case XI_EVIDENCE_REASON_NONE:
            return "none";
        case XI_EVIDENCE_REASON_NOT_ANALYZED:
            return "not_analyzed";
        case XI_EVIDENCE_REASON_IR_REVISION_MISMATCH:
            return "ir_revision_mismatch";
        case XI_EVIDENCE_REASON_CFG_REVISION_MISMATCH:
            return "cfg_revision_mismatch";
        case XI_EVIDENCE_REASON_MEMORY_REVISION_MISMATCH:
            return "memory_revision_mismatch";
        case XI_EVIDENCE_REASON_CALL_REVISION_MISMATCH:
            return "call_revision_mismatch";
        case XI_EVIDENCE_REASON_INVALIDATED_BY_REWRITE:
            return "invalidated_by_rewrite";
        case XI_EVIDENCE_REASON_UNSUPPORTED_SUBJECT:
            return "unsupported_subject";
        case XI_EVIDENCE_REASON_INCOMPLETE_GLOBAL_FACTS:
            return "incomplete_global_facts";
    }
    return "invalid_reason";
}

void xi_evidence_dump(const XiFunc *func, void *file) {
    FILE *out = file ? (FILE *) file : stderr;
    if (!func || !func->evidence)
        return;
    for (uint32_t i = 0; i < XI_EVIDENCE_DOMAIN_COUNT; i++) {
        const XiEvidenceRecord *record = &func->evidence->records[i];
        if (record->id == 0)
            continue;
        XiEvidenceView view = xi_evidence_query(func, record->domain);
        fprintf(out,
                "evidence id=%llu domain=%s state=%s current=%s reason=%s producer=%s "
                "stamp=%llu/%llu/%llu/%llu\n",
                (unsigned long long) record->id, xi_evidence_domain_name(record->domain),
                record->state == XI_PROOF_PROVEN ? "proven" : "unproven",
                view.current ? "yes" : "no", xi_evidence_reason_name(view.reason),
                record->producer ? record->producer : "unknown",
                (unsigned long long) record->stamp.ir_revision,
                (unsigned long long) record->stamp.cfg_revision,
                (unsigned long long) record->stamp.memory_revision,
                (unsigned long long) record->stamp.call_revision);
    }
}
