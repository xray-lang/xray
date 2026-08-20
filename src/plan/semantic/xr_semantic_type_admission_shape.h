/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_type_admission_shape.h - When one frozen type admits a value of
 * another. The two types can belong to two plans, so the canonical key is the
 * only common ground: stable ids are derived from it, and two structurally
 * identical types keyed differently are different types.
 */

#ifndef XR_SEMANTIC_TYPE_ADMISSION_SHAPE_H
#define XR_SEMANTIC_TYPE_ADMISSION_SHAPE_H

#include "xr_semantic_plan.h"
#include <stdbool.h>
#include <string.h>

/* The canonical type key opens with a fixed run of decimal fields. Nullability
 * is the fourth, so a caller that wants to compare two keys while ignoring it
 * needs the offset of that field and of the text after it. */
#define XR_SEMANTIC_TYPE_KEY_PREFIX "type-v3:"
#define XR_SEMANTIC_TYPE_KEY_NULLABLE_FIELD 3u

/* Split a canonical key around its nullability field: `head_length` covers the
 * prefix up to and excluding the field, `value` receives the field itself, and
 * the return value points at the separator that follows it. NULL when the key
 * does not have the expected shape. */
static inline const char *xr_semantic_type_key_split_nullable(const char *key, size_t *head_length,
                                                              unsigned *value) {
    if (!key ||
        strncmp(key, XR_SEMANTIC_TYPE_KEY_PREFIX, sizeof(XR_SEMANTIC_TYPE_KEY_PREFIX) - 1u) != 0)
        return NULL;
    const char *cursor = key + sizeof(XR_SEMANTIC_TYPE_KEY_PREFIX) - 1u;
    for (unsigned field = 0; field < XR_SEMANTIC_TYPE_KEY_NULLABLE_FIELD; field++) {
        const char *digits = cursor;
        while (*cursor >= '0' && *cursor <= '9')
            cursor++;
        if (cursor == digits || *cursor != ':')
            return NULL;
        cursor++;
    }
    if (head_length)
        *head_length = (size_t) (cursor - key);
    const char *digits = cursor;
    unsigned parsed = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        parsed = parsed * 10u + (unsigned) (*cursor - '0');
        cursor++;
    }
    if (cursor == digits || *cursor != ':')
        return NULL;
    if (value)
        *value = parsed;
    return cursor;
}

/* Whether a value of `value_type` may be handed to a parameter of
 * `parameter_type` because the parameter only widens it to its nullable form.
 * The language admits this everywhere -- a definite value is a legal optional --
 * and the frozen plans must agree with the language rather than demand one
 * stable id, because the nullable form is keyed as a distinct type.
 *
 * The widening has to leave the machine representation alone, so it is offered
 * only for a reference-capable type, where null is one of the values the
 * reference already encodes. A nullable scalar carries a separate discriminant
 * and stays unclaimed: admitting it here would silently drop the adapter its
 * call needs. Every other field of the two keys must match exactly, so a type
 * that differs in constness, value semantics, element, name or declaring class
 * is a different type and remains inadmissible. */
static inline bool
xr_semantic_type_is_nullable_widening(const XrSemanticTypeRecord *value_type,
                                      const XrSemanticTypeRecord *parameter_type) {
    if (!value_type || !parameter_type || !value_type->canonical_key ||
        !parameter_type->canonical_key || value_type->kind != parameter_type->kind ||
        (parameter_type->flags & XR_SEM_TYPE_NULLABLE) == 0 ||
        (value_type->flags & XR_SEM_TYPE_NULLABLE) != 0 ||
        (value_type->flags & XR_SEM_TYPE_REFERENCE_CAPABLE) == 0 ||
        (parameter_type->flags & XR_SEM_TYPE_REFERENCE_CAPABLE) == 0)
        return false;
    size_t value_head = 0;
    size_t parameter_head = 0;
    unsigned value_nullable = 0;
    unsigned parameter_nullable = 0;
    const char *value_tail = xr_semantic_type_key_split_nullable(value_type->canonical_key,
                                                                 &value_head, &value_nullable);
    const char *parameter_tail = xr_semantic_type_key_split_nullable(
        parameter_type->canonical_key, &parameter_head, &parameter_nullable);
    return value_tail && parameter_tail && value_nullable == 0u && parameter_nullable == 1u &&
           value_head == parameter_head &&
           strncmp(value_type->canonical_key, parameter_type->canonical_key, value_head) == 0 &&
           strcmp(value_tail, parameter_tail) == 0;
}

/* What a declared parameter admits at a callsite that crosses a module edge.
 *
 * The three layers that check this -- the semantic module-set verifier, the
 * target builder, and the target verifier -- must ask one question, because a
 * call the semantic layer admits and the target layer refuses is reported as a
 * missing target authority, which points at the wrong thing entirely.  The two
 * widenings below are the language's own rules, not this pass's inventions:
 * a value already widens into a nullable reference, and a union parameter
 * admits each of its members. */
static inline bool
xr_semantic_parameter_type_admits_argument(const XrSemanticPlan *callee,
                                           const XrSemanticTypeRecord *parameter_type,
                                           const XrSemanticTypeRecord *operand_type) {
    if (!callee || !parameter_type || !operand_type)
        return false;
    if (xr_stable_id_equal(operand_type->id, parameter_type->id))
        return true;
    if (xr_semantic_type_is_nullable_widening(operand_type, parameter_type))
        return true;
    if (parameter_type->kind != (uint32_t) XR_KIND_UNION)
        return false;
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(callee, &child_count);
    uint32_t type_count = (uint32_t) xr_semantic_plan_type_count(callee);
    for (uint16_t member = 0; member < parameter_type->child_count; member++) {
        uint32_t child = parameter_type->child_begin + member;
        if (!children || child >= child_count)
            return false;
        uint32_t member_type = children[child];
        if (member_type >= type_count)
            return false;
        const XrSemanticTypeRecord *record = xr_semantic_plan_type(callee, member_type);
        if (record && xr_stable_id_equal(operand_type->id, record->id))
            return true;
    }
    return false;
}

#endif  // XR_SEMANTIC_TYPE_ADMISSION_SHAPE_H
