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
#include "../../runtime/value/xtype.h"
#include <stdbool.h>
#include <string.h>

/* The canonical type key opens with a fixed run of decimal fields. Nullability
 * is the fourth and constness the fifth, so a caller that wants to compare two
 * keys while ignoring one qualifier needs the offset of that field and of the
 * text after it. */
#define XR_SEMANTIC_TYPE_KEY_PREFIX "type-v3:"
#define XR_SEMANTIC_TYPE_KEY_NULLABLE_FIELD 3u
#define XR_SEMANTIC_TYPE_KEY_CONST_FIELD 4u

/* Split a canonical key around one decimal field: `head_length` covers the
 * prefix up to and excluding the field, `value` receives the field itself, and
 * the return value points at the separator that follows it. NULL when the key
 * does not have the expected shape. */
static inline const char *xr_semantic_type_key_split_field(const char *key, unsigned target_field,
                                                           size_t *head_length, unsigned *value) {
    if (!key ||
        strncmp(key, XR_SEMANTIC_TYPE_KEY_PREFIX, sizeof(XR_SEMANTIC_TYPE_KEY_PREFIX) - 1u) != 0)
        return NULL;
    const char *cursor = key + sizeof(XR_SEMANTIC_TYPE_KEY_PREFIX) - 1u;
    for (unsigned field = 0; field < target_field; field++) {
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

static inline const char *xr_semantic_type_key_split_nullable(const char *key, size_t *head_length,
                                                              unsigned *value) {
    return xr_semantic_type_key_split_field(key, XR_SEMANTIC_TYPE_KEY_NULLABLE_FIELD, head_length,
                                            value);
}

/* A read parameter cannot mutate the caller's binding. Consequently the
 * caller's top-level const spelling and the declaration's unqualified spelling
 * are one admissible call shape even though they remain separate frozen type
 * rows. No nested qualifier is ignored: every canonical-key byte outside the
 * top-level const field and every other frozen flag must agree. Writable and
 * move parameters must never call this rule. */
static inline bool
xr_semantic_type_is_const_read_admission(const XrSemanticTypeRecord *value_type,
                                         const XrSemanticTypeRecord *parameter_type,
                                         uint8_t parameter_mode) {
    if (!value_type || !parameter_type || !value_type->canonical_key ||
        !parameter_type->canonical_key || parameter_mode != XR_PARAM_READ ||
        value_type->kind != parameter_type->kind ||
        (uint8_t) (value_type->flags | XR_SEM_TYPE_CONST) !=
            (uint8_t) (parameter_type->flags | XR_SEM_TYPE_CONST))
        return false;
    size_t value_head = 0;
    size_t parameter_head = 0;
    unsigned value_const = 0;
    unsigned parameter_const = 0;
    const char *value_tail = xr_semantic_type_key_split_field(
        value_type->canonical_key, XR_SEMANTIC_TYPE_KEY_CONST_FIELD, &value_head, &value_const);
    const char *parameter_tail = xr_semantic_type_key_split_field(
        parameter_type->canonical_key, XR_SEMANTIC_TYPE_KEY_CONST_FIELD, &parameter_head,
        &parameter_const);
    return value_tail && parameter_tail && value_const <= 1u && parameter_const <= 1u &&
           value_head == parameter_head &&
           strncmp(value_type->canonical_key, parameter_type->canonical_key, value_head) == 0 &&
           strcmp(value_tail, parameter_tail) == 0;
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

/* Null has no payload to convert. It may cross a call boundary only when the
 * frozen parameter describes a nullable reference representation. Unknown and
 * value types stay unclaimed even if a malformed row carries those flags. */
static inline bool xr_semantic_null_inhabits_parameter(const XrSemanticTypeRecord *operand_type,
                                                       const XrSemanticTypeRecord *parameter_type) {
    return operand_type && parameter_type && operand_type->kind == (uint32_t) XR_KIND_NULL &&
           parameter_type->kind != (uint32_t) XR_KIND_UNKNOWN &&
           (parameter_type->flags &
            (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE | XR_SEM_TYPE_REFERENCE_CAPABLE)) ==
               (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_REFERENCE_CAPABLE);
}

/* Nullable i64/f64/bool values use the tagged carrier for their whole
 * lifetime: the tag is the optional discriminant and the payload is the scalar.
 * Consequently the null spelling crosses this boundary without an adapter,
 * just like null crossing into a nullable reference. Keep this judgement
 * structural and deliberately narrow. A malformed null row, a non-native
 * scalar width, rune, pointer, reference-capable or aggregate type is not call
 * authority. */
static inline bool
xr_semantic_null_inhabits_nullable_scalar_parameter(const XrSemanticTypeRecord *operand_type,
                                                    const XrSemanticTypeRecord *parameter_type) {
    const uint8_t allowed_parameter_flags = (uint8_t) (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_CONST);
    XrStableId zero = {{0}};
    if (!operand_type || !parameter_type || operand_type->kind != (uint32_t) XR_KIND_NULL ||
        operand_type->builtin_type != XR_TID_NULL ||
        operand_type->scalar_rep != XR_SCALAR_REP_NONE || operand_type->flags != 0 ||
        operand_type->child_count != 0 || operand_type->aggregate_extent != 0 ||
        operand_type->aggregate_align != 0 ||
        operand_type->source_class != XR_SEMANTIC_INDEX_NONE ||
        !xr_stable_id_equal(operand_type->source_class_identity, zero) ||
        operand_type->source_enum_key || operand_type->enum_layout_id != 0 ||
        operand_type->enum_member_count != 0 || operand_type->enum_flags != 0 ||
        operand_type->reserved_enum != 0 ||
        (parameter_type->flags & (uint8_t) ~allowed_parameter_flags) != 0 ||
        (parameter_type->flags & XR_SEM_TYPE_NULLABLE) == 0 ||
        parameter_type->builtin_type != XR_TID_NULL || parameter_type->child_count != 0 ||
        parameter_type->aggregate_extent != 0 || parameter_type->aggregate_align != 0 ||
        parameter_type->source_class != XR_SEMANTIC_INDEX_NONE ||
        !xr_stable_id_equal(parameter_type->source_class_identity, zero) ||
        parameter_type->source_enum_key || parameter_type->enum_layout_id != 0 ||
        parameter_type->enum_member_count != 0 || parameter_type->enum_flags != 0 ||
        parameter_type->reserved_enum != 0)
        return false;
    switch ((XrTypeKind) parameter_type->kind) {
        case XR_KIND_INT:
            return parameter_type->scalar_rep == XR_NATIVE_I64;
        case XR_KIND_FLOAT:
            return parameter_type->scalar_rep == XR_NATIVE_F64;
        case XR_KIND_BOOL:
            return parameter_type->scalar_rep == XR_SCALAR_REP_NONE;
        default:
            return false;
    }
}

/* Widening one exact i64/f64/bool payload into its nullable spelling boxes the
 * scalar into the same tagged carrier used by every nullable scalar value. The
 * backend already freezes that ordinary call-boundary conversion; semantic
 * authority only has to prove that nullable is the sole type difference and
 * that both rows are in the supported scalar family. */
static inline bool
xr_semantic_type_is_nullable_scalar_widening(const XrSemanticTypeRecord *operand_type,
                                             const XrSemanticTypeRecord *parameter_type) {
    XrStableId zero = {{0}};
    if (!operand_type || !parameter_type || !operand_type->canonical_key ||
        !parameter_type->canonical_key || operand_type->kind != parameter_type->kind ||
        operand_type->scalar_rep != parameter_type->scalar_rep ||
        operand_type->builtin_type != XR_TID_NULL || parameter_type->builtin_type != XR_TID_NULL ||
        operand_type->flags != (parameter_type->flags & (uint8_t) ~XR_SEM_TYPE_NULLABLE) ||
        (operand_type->flags & (uint8_t) ~(XR_SEM_TYPE_CONST)) != 0 ||
        (parameter_type->flags & XR_SEM_TYPE_NULLABLE) == 0 || operand_type->child_count != 0 ||
        parameter_type->child_count != 0 || operand_type->aggregate_extent != 0 ||
        operand_type->aggregate_align != 0 || parameter_type->aggregate_extent != 0 ||
        parameter_type->aggregate_align != 0 ||
        operand_type->source_class != XR_SEMANTIC_INDEX_NONE ||
        parameter_type->source_class != XR_SEMANTIC_INDEX_NONE ||
        !xr_stable_id_equal(operand_type->source_class_identity, zero) ||
        !xr_stable_id_equal(parameter_type->source_class_identity, zero) ||
        operand_type->source_enum_key || parameter_type->source_enum_key ||
        operand_type->enum_layout_id != 0 || parameter_type->enum_layout_id != 0 ||
        operand_type->enum_member_count != 0 || parameter_type->enum_member_count != 0 ||
        operand_type->enum_flags != 0 || parameter_type->enum_flags != 0 ||
        operand_type->reserved_enum != 0 || parameter_type->reserved_enum != 0)
        return false;
    switch ((XrTypeKind) parameter_type->kind) {
        case XR_KIND_INT:
            if (parameter_type->scalar_rep != XR_NATIVE_I64)
                return false;
            break;
        case XR_KIND_FLOAT:
            if (parameter_type->scalar_rep != XR_NATIVE_F64)
                return false;
            break;
        case XR_KIND_BOOL:
            if (parameter_type->scalar_rep != XR_SCALAR_REP_NONE)
                return false;
            break;
        default:
            return false;
    }
    size_t operand_head = 0;
    size_t parameter_head = 0;
    unsigned operand_nullable = 0;
    unsigned parameter_nullable = 0;
    const char *operand_tail = xr_semantic_type_key_split_nullable(
        operand_type->canonical_key, &operand_head, &operand_nullable);
    const char *parameter_tail = xr_semantic_type_key_split_nullable(
        parameter_type->canonical_key, &parameter_head, &parameter_nullable);
    return operand_tail && parameter_tail && operand_nullable == 0u && parameter_nullable == 1u &&
           operand_head == parameter_head &&
           strncmp(operand_type->canonical_key, parameter_type->canonical_key, operand_head) == 0 &&
           strcmp(operand_tail, parameter_tail) == 0;
}

/* What a declared parameter admits at a callsite that crosses a module edge.
 *
 * The three layers that check this -- the semantic module-set verifier, the
 * target builder, and the target verifier -- must ask one question, because a
 * call the semantic layer admits and the target layer refuses is reported as a
 * missing target authority, which points at the wrong thing entirely. These
 * admissions are the language's own rules, not this pass's inventions: a
 * read-only boundary admits the top-level const spelling, null inhabits a
 * nullable reference or the exact tagged nullable-scalar family, and a value
 * widens into the nullable form when the carrier or the explicit scalar
 * boundary supplies that representation. A union parameter admits anything
 * admitted by one of its members. */
static inline bool
xr_semantic_parameter_leaf_type_admits_argument(const XrSemanticTypeRecord *parameter_type,
                                                const XrSemanticTypeRecord *operand_type,
                                                uint8_t parameter_mode) {
    if (!parameter_type || !operand_type)
        return false;
    if (xr_stable_id_equal(operand_type->id, parameter_type->id))
        return true;
    if (xr_semantic_type_is_const_read_admission(operand_type, parameter_type, parameter_mode))
        return true;
    if (xr_semantic_null_inhabits_parameter(operand_type, parameter_type))
        return true;
    if (xr_semantic_null_inhabits_nullable_scalar_parameter(operand_type, parameter_type))
        return true;
    if (xr_semantic_type_is_nullable_scalar_widening(operand_type, parameter_type))
        return true;
    if (xr_semantic_type_is_nullable_widening(operand_type, parameter_type))
        return true;
    return false;
}

static inline bool xr_semantic_parameter_type_admits_argument(
    const XrSemanticPlan *callee, const XrSemanticTypeRecord *parameter_type,
    const XrSemanticTypeRecord *operand_type, uint8_t parameter_mode) {
    if (xr_semantic_parameter_leaf_type_admits_argument(parameter_type, operand_type,
                                                        parameter_mode))
        return true;
    if (parameter_type->kind != (uint32_t) XR_KIND_UNION || !callee)
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
        /* Union membership does not erase the ordinary call conversion. A
         * value is admitted when any member would admit it as a parameter in
         * its own right: for example, a read-only const handle may inhabit the
         * corresponding unqualified handle member. */
        if (xr_semantic_parameter_leaf_type_admits_argument(record, operand_type, parameter_mode))
            return true;
    }
    return false;
}

#endif  // XR_SEMANTIC_TYPE_ADMISSION_SHAPE_H
