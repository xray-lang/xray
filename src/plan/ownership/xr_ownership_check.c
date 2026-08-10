/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_ownership_check.c - Independent ownership certificate checker
 */

#include "xr_ownership_check.h"
#include "xr_ownership_certificate_internal.h"
#include "../semantic/xr_semantic_plan_internal.h"
#include "../../base/xmalloc.h"
#include <limits.h>
#include <stdio.h>

static bool report(char *error, size_t size, const char *code, const char *detail) {
    if (error && size)
        snprintf(error, size, "%s: %s", code, detail);
    return false;
}

static const XrOwnershipEdgeStateRecord *find_edge(const XrOwnershipCertificate *certificate,
                                                   uint32_t owner, uint32_t block,
                                                   uint32_t successor) {
    for (uint32_t i = 0; i < certificate->edge_state_count; i++) {
        const XrOwnershipEdgeStateRecord *edge = &certificate->edge_states[i];
        if (edge->owner == owner && edge->block == block && edge->successor == successor)
            return edge;
    }
    return NULL;
}

static const XrOwnershipEdgeStateRecord *find_block_state(const XrOwnershipCertificate *certificate,
                                                          uint32_t owner, uint32_t block) {
    for (uint32_t i = 0; i < certificate->edge_state_count; i++) {
        const XrOwnershipEdgeStateRecord *edge = &certificate->edge_states[i];
        if (edge->owner == owner && edge->block == block)
            return edge;
    }
    return NULL;
}

static bool check_owner_dataflow(const XrSemanticPlan *plan, uint32_t owner_index, char *error,
                                 size_t error_size) {
    const XrOwnershipCertificate *certificate = plan->ownership;
    const XrOwnershipOwnerRecord *owner = &certificate->owners[owner_index];
    if (owner->function >= plan->function_count)
        return report(error, error_size, "XR_OWN_3002", "owner function index is invalid");
    const XrSemanticFunctionRecord *function = &plan->functions[owner->function];
    int32_t *delta = (int32_t *) xr_calloc(function->block_count, sizeof(*delta));
    int32_t *edge_delta =
        (int32_t *) xr_calloc((size_t) function->block_count * 2u, sizeof(*edge_delta));
    if (function->block_count && (!delta || !edge_delta)) {
        xr_free(delta);
        xr_free(edge_delta);
        return report(error, error_size, "XR_EXEC_5003", "checker dataflow budget exhausted");
    }
    for (uint32_t e = 0; e < certificate->event_count; e++) {
        const XrOwnershipEventRecord *event = &certificate->events[e];
        if (event->owner != owner_index)
            continue;
        if (event->block < function->block_begin ||
            event->block >= function->block_begin + function->block_count) {
            xr_free(delta);
            xr_free(edge_delta);
            return report(error, error_size, "XR_OWN_3002", "owner event crosses function");
        }
        uint32_t local = event->block - function->block_begin;
        if (event->successor == XR_SEMANTIC_INDEX_NONE) {
            delta[local] += event->logical_delta;
        } else {
            const XrSemanticBlockRecord *block = &plan->blocks[event->block];
            if (block->successors[0] == event->successor)
                edge_delta[local * 2u] += event->logical_delta;
            else if (block->successors[1] == event->successor)
                edge_delta[local * 2u + 1u] += event->logical_delta;
            else {
                xr_free(delta);
                xr_free(edge_delta);
                return report(error, error_size, "XR_OWN_3002",
                              "owner event references a non-CFG edge");
            }
        }
    }
    for (uint32_t local = 0; local < function->block_count; local++) {
        uint32_t block_index = function->block_begin + local;
        const XrSemanticBlockRecord *block = &plan->blocks[block_index];
        bool terminal = block->successors[0] == XR_SEMANTIC_INDEX_NONE &&
                        block->successors[1] == XR_SEMANTIC_INDEX_NONE;
        for (unsigned s = 0; s < 2; s++) {
            uint32_t successor = terminal ? XR_SEMANTIC_INDEX_NONE : block->successors[s];
            if (!terminal && successor == XR_SEMANTIC_INDEX_NONE)
                continue;
            const XrOwnershipEdgeStateRecord *edge =
                find_edge(certificate, owner_index, block_index, successor);
            int32_t expected_delta = delta[local] + (terminal ? 0 : edge_delta[local * 2u + s]);
            if (!edge || edge->exit_balance - edge->entry_balance != expected_delta) {
                xr_free(delta);
                xr_free(edge_delta);
                return report(error, error_size, "XR_OWN_3001",
                              "certificate edge state does not match independently summed events");
            }
            if ((edge->flags & 1u) == 0 && edge->exit_balance < 0) {
                xr_free(delta);
                xr_free(edge_delta);
                return report(error, error_size, "XR_OWN_3001", "certificate balance is negative");
            }
            if (terminal && (edge->flags & 1u) == 0 && edge->exit_balance != 0) {
                xr_free(delta);
                xr_free(edge_delta);
                return report(error, error_size, "XR_OWN_3001",
                              "certificate ownership is not balanced at a function exit");
            }
            if (!terminal) {
                const XrOwnershipEdgeStateRecord *next =
                    find_block_state(certificate, owner_index, successor);
                /* Ownership dataflow begins at the owner's defining
                 * operation, not at function entry.  CFG edges that precede
                 * that definition are intentionally marked out-of-scope and
                 * do not constrain the definition block's seeded state. */
                if ((edge->flags & 1u) == 0 &&
                    (!next ||
                     ((next->flags & 1u) == 0 && next->entry_balance != edge->exit_balance))) {
                    if (error && error_size) {
                        snprintf(error, error_size,
                                 "XR_OWN_3001: certificate balance is inconsistent across a "
                                 "CFG edge (owner=%s from=%u to=%u exit=%d next-entry=%d "
                                 "next-flags=%u)",
                                 owner->canonical_key, block_index, successor, edge->exit_balance,
                                 next ? next->entry_balance : INT32_MIN,
                                 next ? next->flags : UINT32_MAX);
                    }
                    xr_free(delta);
                    xr_free(edge_delta);
                    return false;
                }
            }
            if (terminal)
                break;
        }
    }
    xr_free(delta);
    xr_free(edge_delta);
    return true;
}

bool xr_ownership_certificate_check(const XrSemanticPlan *plan, char *error, size_t error_size) {
    if (!plan || !plan->frozen || !plan->ownership || !plan->ownership->frozen)
        return report(error, error_size, "XR_OWN_3001",
                      "ownership checker requires a frozen plan and certificate");
    const XrOwnershipCertificate *certificate = plan->ownership;
    if (certificate->schema != plan->schema ||
        !xr_fingerprint_equal(certificate->semantic_fingerprint, plan->fingerprint))
        return report(error, error_size, "XR_OWN_3001",
                      "ownership certificate premise fingerprint does not match the plan");
    for (uint32_t i = 0; i < certificate->event_count; i++) {
        const XrOwnershipEventRecord *event = &certificate->events[i];
        XrStableId expected;
        XrFingerprint digest;
        if (!event->canonical_key ||
            !xr_stable_id_from_key(event->canonical_key, &expected, &digest) ||
            !xr_stable_id_equal(expected, event->id) || event->owner >= certificate->owner_count ||
            event->operation >= plan->operation_count || event->block >= plan->block_count ||
            event->kind > XR_OWN_EVENT_RETURN || event->state_after > XR_OWN_IMMORTAL ||
            (event->successor != XR_SEMANTIC_INDEX_NONE &&
             plan->blocks[event->block].successors[0] != event->successor &&
             plan->blocks[event->block].successors[1] != event->successor))
            return report(error, error_size, "XR_OWN_3002",
                          "ownership event contains an invalid index or state");
    }
    for (uint32_t i = 0; i < certificate->edge_state_count; i++) {
        const XrOwnershipEdgeStateRecord *edge = &certificate->edge_states[i];
        if (edge->owner >= certificate->owner_count || edge->block >= plan->block_count ||
            (edge->successor != XR_SEMANTIC_INDEX_NONE &&
             plan->blocks[edge->block].successors[0] != edge->successor &&
             plan->blocks[edge->block].successors[1] != edge->successor))
            return report(error, error_size, "XR_OWN_3002",
                          "ownership edge state contains an invalid CFG edge");
    }
    for (uint32_t i = 0; i < certificate->owner_count; i++) {
        const XrOwnershipOwnerRecord *owner = &certificate->owners[i];
        XrStableId expected;
        XrFingerprint digest;
        if (!owner->canonical_key ||
            !xr_stable_id_from_key(owner->canonical_key, &expected, &digest) ||
            !xr_stable_id_equal(expected, owner->id) || owner->initial_state > XR_OWN_IMMORTAL ||
            owner->exit_state > XR_OWN_IMMORTAL ||
            !check_owner_dataflow(plan, i, error, error_size))
            return false;
    }
    return true;
}
