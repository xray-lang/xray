/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#include "xi_analysis_manager.h"
#include "xi_effect.h"
#include "xi_escape.h"
#include "xi_memssa.h"
#include "xi_own.h"
#include "xi_range.h"
#include "xi_tbaa.h"
#include "../frontend/analyzer/xa_alloc_effect.h"
#include "../os/os_time.h"
#include "../runtime/value/xtype.h"
#include <stdio.h>
#include <string.h>

static int domain_index(XiEvidenceDomain domain) {
    uint32_t value = (uint32_t) domain;
    if (value == 0 || (value & (value - 1u)) != 0 || value > XI_EVD_MEMSSA)
        return -1;
    int index = 0;
    while ((value >>= 1u) != 0)
        index++;
    return index;
}

static void publish_summary(XiFunc *func, XiEvidenceDomain domain, XiProofState state,
                            XiEvidenceReason reason, XiEvidenceProducer producer, uint64_t first,
                            uint64_t second) {
    XiEvidencePayload payload = {
        .kind = XI_EVIDENCE_PAYLOAD_U64_PAIR,
        .as.u64_pair = {.first = first, .second = second},
    };
    xi_evidence_publish(func, domain, xi_evidence_subject_function(), state, reason, producer, 0,
                        &payload);
}

static void produce_effect(XiFunc *func) {
    uint64_t value_count = 0;
    uint64_t summary = 0;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            XiValue *value = block->values[vi];
            if (!value)
                continue;
            summary |= value->flags;
            value_count++;
            XiEvidencePayload payload = {
                .kind = XI_EVIDENCE_PAYLOAD_U64_PAIR,
                .as.u64_pair = {.first = value->flags, .second = xi_op_semantic_effects(value->op)},
            };
            xi_evidence_publish(func, XI_EVD_EFFECT, xi_evidence_subject_value(value),
                                XI_PROOF_PROVEN, XI_EVIDENCE_REASON_NONE,
                                XI_EVIDENCE_PRODUCER_EFFECT_SCAN, value->line, &payload);
        }
    }
    func->effect_summary = (uint8_t) summary;
    publish_summary(func, XI_EVD_EFFECT, XI_PROOF_PROVEN, XI_EVIDENCE_REASON_NONE,
                    XI_EVIDENCE_PRODUCER_EFFECT_SCAN, summary, value_count);
}

static void publish_ownership_rows(XiFunc *func, const XiOwnResult *own, XiEvidenceDomain domain,
                                   XiEvidenceProducer producer) {
    uint64_t published = 0;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            XiValue *value = block->values[vi];
            if (!value || value->id >= own->max_id)
                continue;
            const XiOwnInfo *info = &own->values[value->id];
            if (!info->rc_managed)
                continue;
            uint64_t first = domain == XI_EVD_OWNERSHIP
                                 ? ((uint64_t) info->ownership | ((uint64_t) info->consumed << 8u) |
                                    ((uint64_t) info->is_dead << 9u))
                                 : ((uint64_t) info->last_use_blk << 32u) | info->last_use_val;
            uint64_t second = domain == XI_EVD_OWNERSHIP
                                  ? info->needs_drop_flag
                                  : ((uint64_t) info->is_dead | ((uint64_t) info->consumed << 1u));
            XiEvidencePayload payload = {
                .kind = XI_EVIDENCE_PAYLOAD_U64_PAIR,
                .as.u64_pair = {.first = first, .second = second},
            };
            xi_evidence_publish(func, domain, xi_evidence_subject_value(value), XI_PROOF_PROVEN,
                                XI_EVIDENCE_REASON_NONE, producer, value->line, &payload);
            published++;
        }
    }
    publish_summary(func, domain, XI_PROOF_PROVEN, XI_EVIDENCE_REASON_NONE, producer, published,
                    domain == XI_EVD_OWNERSHIP ? own->n_drop : 0);
}

static bool produce_ownership_family(XiFunc *func) {
    XiOwnResult own;
    if (!xi_own_analyze(func, &own))
        return false;
    publish_ownership_rows(func, &own, XI_EVD_OWNERSHIP, XI_EVIDENCE_PRODUCER_OWNERSHIP_ANALYSIS);
    publish_ownership_rows(func, &own, XI_EVD_LIFETIME, XI_EVIDENCE_PRODUCER_LIFETIME_ANALYSIS);
    xi_own_free(&own);
    return true;
}

static void produce_noalloc(XiFunc *func) {
    XiProofState state = XI_PROOF_UNPROVEN;
    XiEvidenceReason reason = XI_EVIDENCE_REASON_INCOMPLETE_GLOBAL_FACTS;
    if (func->allocation_effect_complete && func->allocation_state == XA_ALLOC_PROVEN_NONE) {
        state = XI_PROOF_PROVEN;
        reason = XI_EVIDENCE_REASON_NONE;
    }
    publish_summary(func, XI_EVD_NOALLOC, state, reason,
                    XI_EVIDENCE_PRODUCER_ALLOCATION_PUBLICATION, func->allocation_state,
                    func->allocation_reason_bits);
}

static bool value_is_call(const XiValue *value) {
    return value && xi_op_class(value->op) == XI_GEN_CLASS_CALL;
}

static void produce_call_targets(XiFunc *func) {
    uint64_t call_count = 0;
    uint64_t unresolved = 0;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            XiValue *call = block->values[vi];
            if (!value_is_call(call))
                continue;
            call_count++;
            XiEvidenceSubject subject = xi_evidence_subject_callsite(call);
            if (subject.kind == XI_EVIDENCE_SUBJECT_INVALID) {
                unresolved++;
                continue;
            }
            XiEvidencePayload payload = {
                .kind = XI_EVIDENCE_PAYLOAD_U64_PAIR,
                .as.u64_pair = {.first = call->xg_method_id,
                                .second = call->xg_interface_dispatch_slot},
            };
            XiProofState state = call->xg_method_id != 0 || call->op == XI_CALL ? XI_PROOF_PROVEN
                                                                                : XI_PROOF_UNPROVEN;
            XiEvidenceReason reason = state == XI_PROOF_PROVEN
                                          ? XI_EVIDENCE_REASON_NONE
                                          : XI_EVIDENCE_REASON_INCOMPLETE_GLOBAL_FACTS;
            xi_evidence_publish(func, XI_EVD_CALL_TARGET, subject, state, reason,
                                XI_EVIDENCE_PRODUCER_CALL_TARGET_PUBLICATION, call->line, &payload);
            if (state != XI_PROOF_PROVEN)
                unresolved++;
        }
    }
    publish_summary(func, XI_EVD_CALL_TARGET, unresolved == 0 ? XI_PROOF_PROVEN : XI_PROOF_UNPROVEN,
                    unresolved == 0 ? XI_EVIDENCE_REASON_NONE
                                    : XI_EVIDENCE_REASON_INCOMPLETE_GLOBAL_FACTS,
                    XI_EVIDENCE_PRODUCER_CALL_TARGET_PUBLICATION, call_count, unresolved);
}

static void produce_provenance(XiFunc *func) {
    uint64_t pointer_count = 0;
    uint64_t unresolved = 0;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            XiValue *value = block->values[vi];
            if (!value || !value->type || value->type->kind != XR_KIND_POINTER)
                continue;
            pointer_count++;
            uint64_t origin = 0;
            bool proven = false;
            if (value->op == XI_STATIC_ADDR) {
                origin = XR_POINTER_ORIGIN_STATIC;
                proven = true;
            } else if (value->op == XI_CONST && value->aux_int == 0) {
                origin = XR_POINTER_ORIGIN_NULL;
                proven = true;
            } else if (value->op == XI_COPY && value->nargs == 1 && value->args[0]) {
                XiEvidenceView source = xi_evidence_query(
                    func, XI_EVD_PROVENANCE, xi_evidence_subject_value(value->args[0]));
                if (source.current && source.record && source.record->state == XI_PROOF_PROVEN &&
                    source.record->payload.kind == XI_EVIDENCE_PAYLOAD_U64_PAIR) {
                    origin = source.record->payload.as.u64_pair.first;
                    proven = true;
                }
            }
            XiEvidencePayload payload = {
                .kind = XI_EVIDENCE_PAYLOAD_U64_PAIR,
                .as.u64_pair = {.first = origin, .second = value->type->ptr_is_mut},
            };
            xi_evidence_publish(func, XI_EVD_PROVENANCE, xi_evidence_subject_value(value),
                                proven ? XI_PROOF_PROVEN : XI_PROOF_UNPROVEN,
                                proven ? XI_EVIDENCE_REASON_NONE
                                       : XI_EVIDENCE_REASON_INCOMPLETE_GLOBAL_FACTS,
                                XI_EVIDENCE_PRODUCER_PROVENANCE_ANALYSIS, value->line, &payload);
            if (!proven)
                unresolved++;
        }
    }
    publish_summary(func, XI_EVD_PROVENANCE, unresolved == 0 ? XI_PROOF_PROVEN : XI_PROOF_UNPROVEN,
                    unresolved == 0 ? XI_EVIDENCE_REASON_NONE
                                    : XI_EVIDENCE_REASON_INCOMPLETE_GLOBAL_FACTS,
                    XI_EVIDENCE_PRODUCER_PROVENANCE_ANALYSIS, pointer_count, unresolved);
}

static bool produce_domain(XiAnalysisManager *manager, XiEvidenceDomain domain) {
    XiFunc *func = manager->func;
    switch (domain) {
        case XI_EVD_RANGE:
            xi_range_analyze(func);
            break;
        case XI_EVD_ESCAPE:
            xi_escape_analyze(func);
            break;
        case XI_EVD_ALIAS:
            xi_tbaa_annotate(func);
            break;
        case XI_EVD_PROVENANCE:
            produce_provenance(func);
            break;
        case XI_EVD_EFFECT:
            produce_effect(func);
            break;
        case XI_EVD_OWNERSHIP:
        case XI_EVD_LIFETIME:
            if (!produce_ownership_family(func))
                return false;
            break;
        case XI_EVD_NOALLOC:
            produce_noalloc(func);
            break;
        case XI_EVD_CALL_TARGET:
            produce_call_targets(func);
            break;
        case XI_EVD_MEMSSA: {
            XiEvidenceView alias =
                xi_analysis_require(manager, XI_EVD_ALIAS, xi_evidence_subject_function());
            if (!alias.current || !alias.record || alias.record->state != XI_PROOF_PROVEN)
                return false;
            XiMemSSA *memssa = xi_memssa_build(func);
            if (!memssa)
                return false;
            xi_memssa_destroy(memssa);
            break;
        }
        default:
            return false;
    }
    return xi_evidence_domain_is_current(func, domain);
}

void xi_analysis_manager_init(XiAnalysisManager *manager, XiFunc *func) {
    if (!manager)
        return;
    memset(manager, 0, sizeof(*manager));
    manager->func = func;
}

XiEvidenceView xi_analysis_require(XiAnalysisManager *manager, XiEvidenceDomain domain,
                                   XiEvidenceSubject subject) {
    XiEvidenceView empty = {.reason = XI_EVIDENCE_REASON_NOT_ANALYZED};
    int index = domain_index(domain);
    if (!manager || !manager->func || index < 0)
        return empty;
    XiAnalysisProducerStats *stats = &manager->producers[index];
    stats->requests++;
    XiEvidenceView summary =
        xi_evidence_query(manager->func, domain, xi_evidence_subject_function());
    if (!summary.current) {
        uint64_t start = xr_time_monotonic_ns();
        bool produced = produce_domain(manager, domain);
        stats->elapsed_ns += xr_time_monotonic_ns() - start;
        stats->recomputes++;
        if (!produced)
            return empty;
    }
    return xi_evidence_query(manager->func, domain, subject);
}

bool xi_analysis_require_proven_domains(XiAnalysisManager *manager, XiEvidenceDomainMask domains,
                                        char *error, size_t error_size) {
    if (!manager || !manager->func) {
        if (error && error_size)
            snprintf(error, error_size, "analysis manager received a null function");
        return false;
    }
    for (uint32_t bit = 1; bit <= XI_EVD_MEMSSA; bit <<= 1u) {
        if ((domains & bit) == 0)
            continue;
        XiEvidenceDomain domain = (XiEvidenceDomain) bit;
        XiEvidenceView view = xi_analysis_require(manager, domain, xi_evidence_subject_function());
        if (view.current && view.record && view.record->state == XI_PROOF_PROVEN)
            continue;
        if (error && error_size) {
            snprintf(error, error_size, "evidence domain '%s' is unavailable: %s",
                     xi_evidence_domain_name(domain), xi_evidence_reason_name(view.reason));
        }
        return false;
    }
    return true;
}

void xi_analysis_manager_dump(const XiAnalysisManager *manager, void *file) {
    FILE *out = file ? (FILE *) file : stderr;
    if (!manager)
        return;
    for (uint32_t i = 0; i < XI_ANALYSIS_DOMAIN_COUNT; i++) {
        const XiAnalysisProducerStats *stats = &manager->producers[i];
        if (stats->requests == 0)
            continue;
        XiEvidenceDomain domain = (XiEvidenceDomain) (1u << i);
        fprintf(out, "analysis domain=%s requests=%u recomputes=%u elapsed_ns=%llu\n",
                xi_evidence_domain_name(domain), stats->requests, stats->recomputes,
                (unsigned long long) stats->elapsed_ns);
    }
}
