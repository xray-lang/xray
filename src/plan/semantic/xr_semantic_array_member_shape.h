/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_array_member_shape.h - Frozen shape table for array member calls
 *
 * KEY CONCEPT:
 *   An array member call is authorised by its selector, and the selector alone
 *   fixes how many operands the call may carry, which operand holds the element
 *   and what the result is. That is a closed list of facts about the language,
 *   not a conclusion any one layer derives, so it is stated once here and the
 *   semantic builder, the semantic verifier, the target builder and the target
 *   verifier all read this table.
 *
 *   A member that hands back its receiver states RESULT_RECEIVER: its result is
 *   the receiver's own reference rather than a new value, so no row claims
 *   storage for it. A selector absent from the table has no authority at all.
 *
 *   Each layer still reaches its own conclusion from its own records; only the
 *   list of legal shapes is shared.
 */

#ifndef XR_SEMANTIC_ARRAY_MEMBER_SHAPE_H
#define XR_SEMANTIC_ARRAY_MEMBER_SHAPE_H

#include <stddef.h>
#include "xr_semantic_plan.h"
#include "../../ir/xi_ops_gen.h"
#include "../../runtime/value/xtype.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef enum XrArrayMemberResultShape {
    XR_ARRAY_MEMBER_RESULT_UNIT = 0,
    XR_ARRAY_MEMBER_RESULT_INT,
    XR_ARRAY_MEMBER_RESULT_BOOL,
    XR_ARRAY_MEMBER_RESULT_RECEIVER,
} XrArrayMemberResultShape;

typedef struct XrArrayMemberShape {
    const char *selector;
    uint16_t min_operands;
    uint16_t max_operands;
    uint8_t result_shape;
    uint16_t element_operand;
    /* Whether the member may operate on a container of reference-capable
     * elements. A member that only permutes what is already there neither
     * retains nor releases anything, so the element's ownership contract is
     * untouched. One that stores an element into the container does change it,
     * and those stay restricted to elements that own nothing. */
    uint8_t permits_reference_elements;
} XrArrayMemberShape;

/* Operand 0 is always the receiver, so element_operand 0 means the member
 * takes no element of its own. */
static const XrArrayMemberShape xr_array_member_shapes[] = {
    {"push", 2, 2, XR_ARRAY_MEMBER_RESULT_UNIT, 1, 0},
    {"unshift", 2, 2, XR_ARRAY_MEMBER_RESULT_UNIT, 1, 0},
    {"indexOf", 2, 2, XR_ARRAY_MEMBER_RESULT_INT, 1, 0},
    {"contains", 2, 2, XR_ARRAY_MEMBER_RESULT_BOOL, 1, 0},
    {"fill", 2, 4, XR_ARRAY_MEMBER_RESULT_RECEIVER, 1, 0},
    {"reverse", 1, 1, XR_ARRAY_MEMBER_RESULT_RECEIVER, 0, 1},
    {"sort", 1, 1, XR_ARRAY_MEMBER_RESULT_RECEIVER, 0, 1},
};

static inline const XrArrayMemberShape *xr_array_member_shape(const char *selector,
                                                              uint16_t operand_count) {
    if (!selector)
        return NULL;
    for (size_t i = 0; i < sizeof(xr_array_member_shapes) / sizeof(xr_array_member_shapes[0]);
         i++) {
        const XrArrayMemberShape *shape = &xr_array_member_shapes[i];
        if (strcmp(shape->selector, selector) == 0 && operand_count >= shape->min_operands &&
            operand_count <= shape->max_operands)
            return shape;
    }
    return NULL;
}

/* The result a member of each shape must produce.
 *
 * One statement of this, read by both plan layers and by both of their
 * verifiers. Four copies of it used to exist -- two per layer -- and the
 * receiver-returning branch was wrong in all four the same way, which is why
 * `reverse`, `sort` and `fill` never matched: it demanded an OWNED result while
 * the same judgement also required the result to alias operand 0. A member that
 * hands the receiver back returns a borrow of the container the caller already
 * owns; the alias and the ownership now say the same thing. */
static inline bool xr_semantic_array_member_unit_type_is_exact(const XrSemanticTypeRecord *type) {
    char expected[96];
    int length = snprintf(expected, sizeof(expected),
                          "type-v3:%u:0:%u:0:0:0:0:0:0:%u:0:", (unsigned) XR_KIND_UNIT,
                          (unsigned) XR_TID_NULL, (unsigned) XR_SCALAR_REP_NONE);
    return type && length > 0 && (size_t) length < sizeof(expected) && type->kind == XR_KIND_UNIT &&
           type->builtin_type == XR_TID_NULL && type->child_count == 0 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE && type->flags == 0 && type->canonical_key &&
           strcmp(type->canonical_key, expected) == 0;
}

static inline bool xr_semantic_array_member_i64_type_is_exact(const XrSemanticTypeRecord *type) {
    return type && type->kind == XR_KIND_INT && type->builtin_type == XR_TID_NULL &&
           type->child_count == 0 && type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_NATIVE_I64 && type->flags == 0;
}

static inline bool xr_semantic_array_member_bool_type_is_exact(const XrSemanticTypeRecord *type) {
    return type && type->kind == XR_KIND_BOOL && type->builtin_type == XR_TID_NULL &&
           type->child_count == 0 && type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE && type->flags == 0;
}

static inline bool xr_semantic_array_member_result_is_exact(
    const XrSemanticOperationRecord *operation, const XrArrayMemberShape *shape,
    const XrSemanticTypeRecord *result_type, uint32_t receiver_type_index) {
    if (!operation || !shape)
        return false;
    if (shape->result_shape == XR_ARRAY_MEMBER_RESULT_RECEIVER) {
        /* The invariant is the alias, not the ownership word: the result is
         * operand 0 itself. Lowering states that fact as an owner when the
         * receiver is one the caller already owns outright and as a borrow when
         * it reads the container through a shared root, so both pairings are
         * the same fact about the same value and both are admitted -- but only
         * as pairs, because an owner with a borrowed provenance (or the
         * reverse) would be two different claims about one result. */
        bool owned = operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
                     operation->return_provenance == XR_SEM_RETURN_OWNED;
        bool borrowed = operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
                        operation->return_provenance == XR_SEM_RETURN_BORROWED_STATIC;
        return operation->result_type == receiver_type_index &&
               operation->result_alias_operand == 0 && (owned || borrowed) &&
               operation->return_parameter == -1 && operation->return_complete == 1;
    }
    bool typed = shape->result_shape == XR_ARRAY_MEMBER_RESULT_UNIT
                     ? xr_semantic_array_member_unit_type_is_exact(result_type)
                 : shape->result_shape == XR_ARRAY_MEMBER_RESULT_INT
                     ? xr_semantic_array_member_i64_type_is_exact(result_type)
                     : xr_semantic_array_member_bool_type_is_exact(result_type);
    return typed && operation->result_alias_operand == -1 &&
           operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_CALL_RESULT &&
           operation->return_provenance == XR_SEM_RETURN_NONE &&
           operation->return_parameter == -1 && operation->return_complete == 0;
}

#endif  // XR_SEMANTIC_ARRAY_MEMBER_SHAPE_H
