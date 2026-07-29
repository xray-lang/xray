/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#include "xi_edit.h"
#include "xi_analysis.h"
#include "xi_effect.h"
#include "xi_tbaa.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define FNV_OFFSET UINT64_C(1469598103934665603)
#define FNV_PRIME UINT64_C(1099511628211)

static uint64_t hash_u64(uint64_t hash, uint64_t value) {
    for (uint32_t i = 0; i < 8; i++) {
        hash ^= (uint8_t) (value >> (i * 8u));
        hash *= FNV_PRIME;
    }
    return hash;
}

static uint64_t value_semantic_hash(uint64_t hash, const XiValue *value, bool include_type) {
    if (!value)
        return hash_u64(hash, UINT64_MAX);
    hash = hash_u64(hash, value->id);
    hash = hash_u64(hash, value->op);
    hash = hash_u64(hash, value->flags);
    hash = hash_u64(hash, value->nargs);
    hash = hash_u64(hash, (uint64_t) value->aux_int);
    hash = hash_u64(hash, (uintptr_t) value->aux);
    hash = hash_u64(hash, value->var_id);
    hash = hash_u64(hash, value->lowering_flags);
    hash = hash_u64(hash, (uint64_t) value->conversion.kind);
    hash = hash_u64(hash, value->conversion.source_scalar_rep);
    hash = hash_u64(hash, value->conversion.target_scalar_rep);
    hash = hash_u64(hash, value->conversion.is_implicit ? 1u : 0u);
    hash = hash_u64(hash, value->conversion.is_compile_time ? 1u : 0u);
    hash = hash_u64(hash, value->xa_intrinsic_id);
    hash = hash_u64(hash, value->xg_callsite_id);
    hash = hash_u64(hash, value->xg_method_id);
    hash = hash_u64(hash, value->xg_interface_dispatch_slot);
    if (include_type) {
        hash = hash_u64(hash, (uintptr_t) value->type);
        hash = hash_u64(hash, value->rep);
    }
    for (uint16_t i = 0; i < value->nargs; i++)
        hash = hash_u64(hash, value->args[i] ? value->args[i]->id : UINT32_MAX);
    return hash;
}

static uint64_t value_type_hash(uint64_t hash, const XiValue *value) {
    if (!value)
        return hash_u64(hash, UINT64_MAX);
    hash = hash_u64(hash, value->id);
    hash = hash_u64(hash, (uintptr_t) value->type);
    return hash_u64(hash, value->rep);
}

static bool value_touches_memory(const XiValue *value) {
    return value && ((value->flags & XI_FLAG_MEM_ANY) != 0 || xi_is_memory_op(value->op));
}

static bool value_is_call(const XiValue *value) {
    return value && xi_op_class(value->op) == XI_GEN_CLASS_CALL;
}

XiEditFingerprint xi_edit_fingerprint(const XiFunc *func) {
    XiEditFingerprint result = {
        .cfg = FNV_OFFSET,
        .values = FNV_OFFSET,
        .types = FNV_OFFSET,
        .memory = FNV_OFFSET,
        .calls = FNV_OFFSET,
    };
    if (!func)
        return result;
    result.cfg = hash_u64(result.cfg, func->nblocks);
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        result.cfg = hash_u64(result.cfg, block->id);
        result.cfg = hash_u64(result.cfg, block->kind);
        result.cfg = hash_u64(result.cfg, block->succs[0] ? block->succs[0]->id : UINT32_MAX);
        result.cfg = hash_u64(result.cfg, block->succs[1] ? block->succs[1]->id : UINT32_MAX);
        result.cfg = hash_u64(result.cfg, block->npreds);
        for (uint16_t pi = 0; pi < block->npreds; pi++)
            result.cfg = hash_u64(result.cfg, block->preds[pi] ? block->preds[pi]->id : UINT32_MAX);
        for (const XiPhi *phi = block->phis; phi; phi = phi->next) {
            result.values = value_semantic_hash(result.values, &phi->value, false);
            result.types = value_type_hash(result.types, &phi->value);
        }
        result.values = hash_u64(result.values, block->control ? block->control->id : UINT32_MAX);
        result.values = hash_u64(result.values, block->nvalues);
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            const XiValue *value = block->values[vi];
            result.values = value_semantic_hash(result.values, value, false);
            result.types = value_type_hash(result.types, value);
            if (value_touches_memory(value))
                result.memory = value_semantic_hash(result.memory, value, true);
            if (value_is_call(value))
                result.calls = value_semantic_hash(result.calls, value, true);
        }
    }
    return result;
}

bool xi_edit_begin(XiEditSession *session, XiFunc *func) {
    if (!session || !func)
        return false;
    memset(session, 0, sizeof(*session));
    session->func = func;
    session->before = xi_edit_fingerprint(func);
    session->ir_revision = func->ir_revision;
    session->cfg_revision = func->cfg_version;
    session->memory_revision = func->memory_revision;
    session->call_revision = func->call_revision;
    session->active = true;
    return true;
}

static bool fail(char *error, size_t error_size, const char *format, ...) {
    if (error && error_size) {
        va_list args;
        va_start(args, format);
        vsnprintf(error, error_size, format, args);
        va_end(args);
    }
    return false;
}

bool xi_edit_finish(XiEditSession *session, XiPassChange reported,
                    XiEvidenceDomainMask explicitly_invalidates,
                    XiEvidenceDomainMask declared_preserves, XiPassOutcome *outcome, char *error,
                    size_t error_size) {
    if (!session || !session->active || !session->func || !outcome)
        return fail(error, error_size, "invalid or inactive Xi edit session");
    XiFunc *func = session->func;
    XiEditFingerprint after = xi_edit_fingerprint(func);
    bool cfg_changed = session->before.cfg != after.cfg;
    bool values_changed = session->before.values != after.values;
    bool types_changed = session->before.types != after.types && !values_changed;
    bool memory_changed = session->before.memory != after.memory;
    bool calls_changed = session->before.calls != after.calls;

    if (cfg_changed && !reported.cfg_changed)
        return fail(error, error_size, "pass mutated CFG without reporting cfg_changed");
    if (values_changed && !reported.values_changed && !reported.cfg_changed)
        return fail(error, error_size, "pass mutated values without reporting values_changed");
    if (types_changed && !reported.types_changed && !reported.values_changed &&
        !reported.cfg_changed)
        return fail(error, error_size, "pass mutated types without reporting types_changed");
    if (func->ir_revision != session->ir_revision)
        return fail(error, error_size, "pass changed IR revision outside XiEditSession");
    if (func->cfg_version != session->cfg_revision && !reported.cfg_changed)
        return fail(error, error_size, "pass changed CFG revision without reporting cfg_changed");
    if (func->memory_revision != session->memory_revision ||
        func->call_revision != session->call_revision)
        return fail(error, error_size, "pass changed evidence revisions outside XiEditSession");

    bool any_cfg = cfg_changed || reported.cfg_changed;
    bool any_values = values_changed || reported.values_changed;
    bool any_types = types_changed || reported.types_changed;
    XiEvidenceDomainMask invalidates = explicitly_invalidates;
    if (any_cfg)
        invalidates |=
            XI_EVD_RANGE | XI_EVD_ESCAPE | XI_EVD_OWNERSHIP | XI_EVD_LIFETIME | XI_EVD_MEMSSA;
    if (any_values)
        invalidates |= XI_EVD_RANGE | XI_EVD_EFFECT | XI_EVD_ESCAPE | XI_EVD_OWNERSHIP |
                       XI_EVD_LIFETIME | XI_EVD_NOALLOC;
    if (memory_changed)
        invalidates |= XI_EVD_ALIAS | XI_EVD_PROVENANCE | XI_EVD_MEMSSA;
    if (calls_changed)
        invalidates |= XI_EVD_CALL_TARGET | XI_EVD_EFFECT | XI_EVD_ESCAPE | XI_EVD_NOALLOC |
                       XI_EVD_ALIAS | XI_EVD_MEMSSA | XI_EVD_OWNERSHIP | XI_EVD_LIFETIME;
    if (any_types)
        invalidates |= XI_EVD_ALL;
    invalidates &= ~declared_preserves;
    XiEvidenceDomainMask preserves = XI_EVD_ALL & ~invalidates;

    if (any_cfg && func->cfg_version == session->cfg_revision)
        xi_cfg_invalidate(func);
    if (any_cfg || any_values || any_types) {
        XiEvidenceStamp prior_stamp = {
            .ir_revision = session->ir_revision,
            .cfg_revision = session->cfg_revision,
            .memory_revision = session->memory_revision,
            .call_revision = session->call_revision,
        };
        xi_evidence_note_rewrite(func, any_cfg, any_values, any_types, invalidates);
        xi_evidence_rebase_preserved(func, preserves, prior_stamp);
    }

    *outcome = (XiPassOutcome) {
        .change = reported,
        .invalidates = invalidates,
        .preserves = preserves,
        .revision_delta =
            {
                .ir_changed = func->ir_revision != session->ir_revision,
                .cfg_changed = func->cfg_version != session->cfg_revision,
                .memory_changed = func->memory_revision != session->memory_revision,
                .call_changed = func->call_revision != session->call_revision,
            },
    };
    session->active = false;
    return true;
}
