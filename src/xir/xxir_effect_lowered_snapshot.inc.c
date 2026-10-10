/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_lowered_snapshot.inc.c - Exact copied physical type correspondence
 *
 * KEY CONCEPT:
 *   This comparison proves only that a full verified Lowered pool was copied.
 *   Identity-only tables never become semantic declaration or access proofs.
 */
#include "xxir_types.h"
#include "xxir_compile_memory.h"

static XrXirStatus effect_lowered_snapshot_literal(const XrXirCompileContext *work,
    XrXirLiteral left, XrXirLiteral right) {
    if (!xir_compile_work(work, 1)) return XR_XIR_BUDGET;
    if (left.length != right.length || (left.length && (!left.bytes || !right.bytes)))
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work, (uint64_t)left.length * 2)) return XR_XIR_BUDGET;
    return !left.length || !memcmp(left.bytes, right.bytes, left.length) ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}

static XrXirStatus effect_lowered_snapshot_vector(const XrXirCompileContext *work,
    const XrXirType *left, const XrXirType *right, uint32_t count) {
    if (count && (!left || !right)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i = 0; i < count; ++i) {
        if (!xir_compile_work(work, 2 * sizeof(*left))) return XR_XIR_BUDGET;
        if (left[i] != right[i]) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_lowered_snapshot_node(const XrXirCompileContext *work,
    const XrXirTypeNode *left, const XrXirTypeNode *right) {
    if (!xir_compile_work(work, 2 * sizeof(*left))) return XR_XIR_BUDGET;
    if (left->kind != right->kind || left->element != right->element ||
        left->result != right->result || left->flags != right->flags ||
        left->parameter_span != right->parameter_span || left->parameter_span ||
        left->parameter_count != right->parameter_count ||
        left->nominal.declaration != right->nominal.declaration ||
        left->nominal.argument_count != right->nominal.argument_count ||
        left->nominal.field_count != right->nominal.field_count ||
        (left->parameter_count && (!left->parameters || !right->parameters)))
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p = 0; p < left->parameter_count; ++p) {
        if (!xir_compile_work(work, 2 * sizeof(*left->parameters))) return XR_XIR_BUDGET;
        if (left->parameters[p].mode != right->parameters[p].mode ||
            left->parameters[p].type != right->parameters[p].type) return XR_XIR_BAD_STRUCTURE;
    }
    XrXirStatus status = effect_lowered_snapshot_vector(work, left->nominal.arguments,
        right->nominal.arguments, left->nominal.argument_count);
    if (status == XR_XIR_OK) status = effect_lowered_snapshot_vector(work, left->nominal.fields,
        right->nominal.fields, left->nominal.field_count);
    return status;
}

static XrXirStatus effect_lowered_snapshot_identity(const XrXirCompileContext *work,
    const XrXirNominalIdentity *left, const XrXirNominalIdentity *right) {
    if (!xir_compile_work(work, 2 * sizeof(*left))) return XR_XIR_BUDGET;
    if (left->exported != right->exported || left->arity != right->arity ||
        left->kind != right->kind || left->flags != right->flags ||
        left->field_count != right->field_count || left->variant_count != right->variant_count ||
        left->native.native_id != right->native.native_id ||
        memcmp(left->native.source_fingerprint, right->native.source_fingerprint, 32) ||
        (left->field_count && (!left->fields || !right->fields)) ||
        (left->variant_count && (!left->variants || !right->variants))) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = effect_lowered_snapshot_literal(work, left->module, right->module);
    if (status == XR_XIR_OK) status = effect_lowered_snapshot_literal(work, left->name, right->name);
    for (uint32_t f = 0; f < left->field_count && status == XR_XIR_OK; ++f) {
        if (!xir_compile_work(work, 2 * sizeof(*left->fields))) return XR_XIR_BUDGET;
        if (left->fields[f].flags != right->fields[f].flags) return XR_XIR_BAD_STRUCTURE;
        status = effect_lowered_snapshot_literal(work, left->fields[f].name, right->fields[f].name);
    }
    for (uint32_t v = 0; v < left->variant_count && status == XR_XIR_OK; ++v) {
        if (!xir_compile_work(work, 2 * sizeof(*left->variants))) return XR_XIR_BUDGET;
        if (left->variants[v].field_begin != right->variants[v].field_begin ||
            left->variants[v].field_count != right->variants[v].field_count)
            return XR_XIR_BAD_STRUCTURE;
        status = effect_lowered_snapshot_literal(work, left->variants[v].name, right->variants[v].name);
    }
    return status;
}

static XrXirStatus effect_lowered_snapshot_nominals(const XrXirCompileContext *work,
    const XrXirNominalTable *left, const XrXirNominalTable *right) {
    if (!xir_compile_work(work, 2 * sizeof(XrXirNominalTable))) return XR_XIR_BUDGET;
    if (!!left != !!right) return XR_XIR_BAD_STRUCTURE;
    if (!left) return XR_XIR_OK;
    if (left->declarations || right->declarations || left->count != right->count ||
        !left->count || !left->identities || !right->identities) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t n = 0; n < left->count; ++n) {
        XrXirStatus status = effect_lowered_snapshot_identity(work,
            &left->identities[n], &right->identities[n]);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

/* Equal exact child IDs are compared across every node in the same order.
 * No recursive structural equality, pointer trust or declaration proof runs.
 * Full source verification precedes cloning. The comparison grants no semantic
 * or executable authority and never replaces that verification. */
static XrXirStatus effect_lowered_snapshot_match(const XrXirCompileContext *work,
    const XrXirTypes *owned, const XrXirTypes *source) {
    if (!xir_compile_context_valid(work) || !owned || !source) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work, 2 * sizeof(*owned))) return XR_XIR_BUDGET;
    if (owned->count != source->count ||
        owned->count > XR_XIR_CONSTRUCTED_TYPE_LIMIT - XR_XIR_CONSTRUCTED_TYPE_BASE ||
        ((owned->count != 0) != (owned->nodes != NULL)) ||
        ((source->count != 0) != (source->nodes != NULL)) ||
        owned->interfaces || source->interfaces) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = effect_lowered_snapshot_nominals(work, owned->nominals, source->nominals);
    for (uint32_t n = 0; n < owned->count && status == XR_XIR_OK; ++n)
        status = effect_lowered_snapshot_node(work, &owned->nodes[n], &source->nodes[n]);
    return status;
}
