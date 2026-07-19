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

struct XiEvidenceSet {
    XiEvidenceRecord **records;
    uint32_t count;
    uint32_t capacity;
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
    if (func->evidence) {
        for (uint32_t i = 0; i < func->evidence->count; i++)
            xr_free(func->evidence->records[i]);
        xr_free(func->evidence->records);
        xr_free(func->evidence);
    }
    func->evidence = NULL;
}

XiEvidenceSubject xi_evidence_subject_function(void) {
    return (XiEvidenceSubject) {.kind = XI_EVIDENCE_SUBJECT_FUNCTION, .id = 0};
}

XiEvidenceSubject xi_evidence_subject_value(const XiValue *value) {
    if (!value)
        return (XiEvidenceSubject) {0};
    return (XiEvidenceSubject) {.kind = XI_EVIDENCE_SUBJECT_VALUE, .id = value->id};
}

XiEvidenceSubject xi_evidence_subject_callsite(const XiValue *call) {
    if (!call || call->xg_callsite_id == 0)
        return (XiEvidenceSubject) {0};
    return (XiEvidenceSubject) {
        .kind = XI_EVIDENCE_SUBJECT_CALLSITE,
        .id = call->xg_callsite_id,
    };
}

static bool subject_equal(XiEvidenceSubject lhs, XiEvidenceSubject rhs) {
    return lhs.kind == rhs.kind && lhs.id == rhs.id;
}

static bool subject_valid(XiEvidenceSubject subject) {
    return subject.kind > XI_EVIDENCE_SUBJECT_INVALID &&
           subject.kind <= XI_EVIDENCE_SUBJECT_CALLSITE;
}

static XiEvidenceRecord *find_record(const XiFunc *func, XiEvidenceDomain domain,
                                     XiEvidenceSubject subject) {
    if (!func || !func->evidence)
        return NULL;
    for (uint32_t i = func->evidence->count; i > 0; i--) {
        XiEvidenceRecord *record = func->evidence->records[i - 1];
        if (record && record->domain == domain && subject_equal(record->subject, subject))
            return record;
    }
    return NULL;
}

static XiEvidenceRecord *append_record(struct XiEvidenceSet *set) {
    if (set->count == set->capacity) {
        uint32_t capacity = set->capacity == 0 ? 16 : set->capacity * 2;
        XiEvidenceRecord **records =
            (XiEvidenceRecord **) xr_realloc(set->records, capacity * sizeof(*records));
        if (!records)
            return NULL;
        set->records = records;
        set->capacity = capacity;
    }
    XiEvidenceRecord *record = (XiEvidenceRecord *) xr_calloc(1, sizeof(*record));
    if (!record)
        return NULL;
    set->records[set->count++] = record;
    return record;
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
                                            XiEvidenceSubject subject, XiProofState state,
                                            XiEvidenceReason reason, XiEvidenceProducer producer,
                                            uint32_t source_line,
                                            const XiEvidencePayload *payload) {
    int index = domain_index(domain);
    if (!func || !func->evidence || index < 0 || !subject_valid(subject) ||
        state == XI_PROOF_INVALID || producer == XI_EVIDENCE_PRODUCER_INVALID)
        return NULL;
    /* Evidence rows are immutable publications. A recomputation creates a new
     * ID so downstream plans can retain an auditable link to the exact proof
     * they consumed. */
    XiEvidenceRecord *record = append_record(func->evidence);
    if (!record)
        return NULL;
    memset(record, 0, sizeof(*record));
    record->id = func->evidence->next_id++;
    record->domain = domain;
    record->subject = subject;
    record->state = state;
    record->reason = reason;
    record->producer = producer;
    record->source_file = func->source_file;
    record->source_line = source_line;
    if (payload)
        record->payload = *payload;
    record->stamp = xi_evidence_current_stamp(func);
    return record;
}

const XiEvidenceRecord *xi_evidence_find_by_id(const XiFunc *func, XiEvidenceId id) {
    if (!func || !func->evidence || id == 0)
        return NULL;
    for (uint32_t i = 0; i < func->evidence->count; i++) {
        const XiEvidenceRecord *record = func->evidence->records[i];
        if (record && record->id == id)
            return record;
    }
    return NULL;
}

XiEvidenceView xi_evidence_query(const XiFunc *func, XiEvidenceDomain domain,
                                 XiEvidenceSubject subject) {
    XiEvidenceView view = {0};
    int index = domain_index(domain);
    if (!func || !func->evidence || index < 0 || !subject_valid(subject)) {
        view.reason = XI_EVIDENCE_REASON_NOT_ANALYZED;
        return view;
    }
    view.record = find_record(func, domain, subject);
    if (!view.record) {
        view.reason = XI_EVIDENCE_REASON_SUBJECT_NOT_FOUND;
        return view;
    }
    XiEvidenceReason freshness = stale_reason(func, view.record);
    view.current = freshness == XI_EVIDENCE_REASON_NONE;
    view.reason = view.current ? view.record->reason : freshness;
    return view;
}

bool xi_evidence_is_current(const XiFunc *func, XiEvidenceDomain domain,
                            XiEvidenceSubject subject) {
    return xi_evidence_query(func, domain, subject).current;
}

bool xi_evidence_is_proven_current(const XiFunc *func, XiEvidenceDomain domain,
                                   XiEvidenceSubject subject) {
    XiEvidenceView view = xi_evidence_query(func, domain, subject);
    return view.current && view.record && view.record->state == XI_PROOF_PROVEN;
}

bool xi_evidence_domain_is_current(const XiFunc *func, XiEvidenceDomain domain) {
    return xi_evidence_is_current(func, domain, xi_evidence_subject_function());
}

bool xi_evidence_domain_is_proven_current(const XiFunc *func, XiEvidenceDomain domain) {
    return xi_evidence_is_proven_current(func, domain, xi_evidence_subject_function());
}

void xi_evidence_invalidate(XiFunc *func, XiEvidenceDomainMask domains, XiEvidenceReason reason) {
    if (!func || !func->evidence)
        return;
    for (uint32_t i = 0; i < func->evidence->count; i++) {
        XiEvidenceRecord *record = func->evidence->records[i];
        if (((uint32_t) record->domain & domains) == 0)
            continue;
        record->state = XI_PROOF_INVALID;
        record->reason = reason;
    }
}

static bool value_subject_exists(const XiFunc *func, uint64_t id) {
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        for (const XiPhi *phi = block->phis; phi; phi = phi->next) {
            if (phi->value.id == id)
                return true;
        }
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            if (block->values[vi] && block->values[vi]->id == id)
                return true;
        }
    }
    return false;
}

static bool callsite_subject_exists(const XiFunc *func, uint64_t id) {
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            if (block->values[vi] && block->values[vi]->xg_callsite_id == id)
                return true;
        }
    }
    return false;
}

void xi_evidence_prune_orphans(XiFunc *func) {
    if (!func || !func->evidence)
        return;
    uint32_t write = 0;
    for (uint32_t read = 0; read < func->evidence->count; read++) {
        XiEvidenceRecord *record = func->evidence->records[read];
        bool exists = record->subject.kind == XI_EVIDENCE_SUBJECT_FUNCTION;
        if (record->subject.kind == XI_EVIDENCE_SUBJECT_VALUE)
            exists = value_subject_exists(func, record->subject.id);
        else if (record->subject.kind == XI_EVIDENCE_SUBJECT_CALLSITE)
            exists = callsite_subject_exists(func, record->subject.id);
        if (!exists) {
            xr_free(record);
            continue;
        }
        func->evidence->records[write++] = record;
    }
    func->evidence->count = write;
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
    xi_evidence_prune_orphans(func);
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
        case XI_EVIDENCE_REASON_SUBJECT_NOT_FOUND:
            return "subject_not_found";
        case XI_EVIDENCE_REASON_SUBJECT_DELETED:
            return "subject_deleted";
        case XI_EVIDENCE_REASON_PRODUCER_UNAVAILABLE:
            return "producer_unavailable";
        case XI_EVIDENCE_REASON_INCOMPLETE_GLOBAL_FACTS:
            return "incomplete_global_facts";
    }
    return "invalid_reason";
}

const char *xi_evidence_subject_kind_name(XiEvidenceSubjectKind kind) {
    switch (kind) {
        case XI_EVIDENCE_SUBJECT_FUNCTION:
            return "function";
        case XI_EVIDENCE_SUBJECT_VALUE:
            return "value";
        case XI_EVIDENCE_SUBJECT_CALLSITE:
            return "callsite";
        case XI_EVIDENCE_SUBJECT_INVALID:
            break;
    }
    return "invalid";
}

const char *xi_evidence_producer_name(XiEvidenceProducer producer) {
    switch (producer) {
        case XI_EVIDENCE_PRODUCER_RANGE_ANALYSIS:
            return "range_analysis";
        case XI_EVIDENCE_PRODUCER_ESCAPE_ANALYSIS:
            return "escape_analysis";
        case XI_EVIDENCE_PRODUCER_TBAA:
            return "tbaa";
        case XI_EVIDENCE_PRODUCER_MEMSSA:
            return "memssa";
        case XI_EVIDENCE_PRODUCER_EFFECT_SCAN:
            return "effect_scan";
        case XI_EVIDENCE_PRODUCER_OWNERSHIP_ANALYSIS:
            return "ownership_analysis";
        case XI_EVIDENCE_PRODUCER_LIFETIME_ANALYSIS:
            return "lifetime_analysis";
        case XI_EVIDENCE_PRODUCER_ALLOCATION_PUBLICATION:
            return "allocation_publication";
        case XI_EVIDENCE_PRODUCER_CALL_TARGET_PUBLICATION:
            return "call_target_publication";
        case XI_EVIDENCE_PRODUCER_PROVENANCE_ANALYSIS:
            return "provenance_analysis";
        case XI_EVIDENCE_PRODUCER_TEST:
            return "test";
        case XI_EVIDENCE_PRODUCER_INVALID:
            break;
    }
    return "invalid";
}

void xi_evidence_dump(const XiFunc *func, void *file) {
    FILE *out = file ? (FILE *) file : stderr;
    if (!func || !func->evidence)
        return;
    for (uint32_t i = 0; i < func->evidence->count; i++) {
        const XiEvidenceRecord *record = func->evidence->records[i];
        XiEvidenceView view = xi_evidence_query(func, record->domain, record->subject);
        fprintf(out,
                "evidence id=%llu domain=%s subject=%s:%llu state=%s current=%s reason=%s "
                "producer=%s source=%s:%u stamp=%llu/%llu/%llu/%llu\n",
                (unsigned long long) record->id, xi_evidence_domain_name(record->domain),
                xi_evidence_subject_kind_name(record->subject.kind),
                (unsigned long long) record->subject.id,
                record->state == XI_PROOF_PROVEN ? "proven" : "unproven",
                view.current ? "yes" : "no", xi_evidence_reason_name(view.reason),
                xi_evidence_producer_name(record->producer),
                record->source_file ? record->source_file : "<unknown>", record->source_line,
                (unsigned long long) record->stamp.ir_revision,
                (unsigned long long) record->stamp.cfg_revision,
                (unsigned long long) record->stamp.memory_revision,
                (unsigned long long) record->stamp.call_revision);
    }
}
