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
#include "xr_semantic_array_type_shape.h"
#include "xr_semantic_class_shape.h"
#include "xr_semantic_string_shape.h"
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
    XR_ARRAY_MEMBER_RESULT_STRING,
} XrArrayMemberResultShape;

typedef enum XrArrayMemberElementAccess {
    XR_ARRAY_MEMBER_ELEMENT_ACCESS_NONE = 0,
    XR_ARRAY_MEMBER_ELEMENT_ACCESS_READ,
    XR_ARRAY_MEMBER_ELEMENT_ACCESS_MOVE,
    XR_ARRAY_MEMBER_ELEMENT_ACCESS_STORE,
} XrArrayMemberElementAccess;

typedef enum XrArrayMemberReferenceAction {
    XR_ARRAY_MEMBER_REFERENCE_UNSUPPORTED = 0,
    XR_ARRAY_MEMBER_REFERENCE_PRESERVE,
    XR_ARRAY_MEMBER_REFERENCE_CONSUME_INTO_STORAGE,
} XrArrayMemberReferenceAction;

typedef enum XrArrayMemberReferenceDrop {
    XR_ARRAY_MEMBER_REFERENCE_DROP_NONE = 0,
    XR_ARRAY_MEMBER_REFERENCE_DROP_RELEASE_ON_ERASE_OR_DESTROY,
} XrArrayMemberReferenceDrop;

typedef struct XrArrayMemberShape {
    const char *selector;
    uint16_t min_operands;
    uint16_t max_operands;
    uint8_t result_shape;
    uint16_t element_operand;
    /* Reference-capable elements need a complete lifecycle rather than a
     * permission bit. Access says whether the member reads, moves, or stores an
     * element; action says who owns a reference after the member; drop says how
     * that ownership eventually ends. Unsupported rows remain fail-closed. */
    uint8_t element_access;
    uint8_t reference_action;
    uint8_t reference_drop;
    /* Which operand, if any, is a plain string rather than the i64 bound every
     * other non-element argument is. Zero means none, since operand 0 is always
     * the receiver. */
    uint16_t string_operand;
} XrArrayMemberShape;

/* Operand 0 is always the receiver, so element_operand 0 means the member
 * takes no element of its own. */
static const XrArrayMemberShape xr_array_member_shapes[] = {
    {"push", 2, 2, XR_ARRAY_MEMBER_RESULT_UNIT, 1, XR_ARRAY_MEMBER_ELEMENT_ACCESS_STORE,
     XR_ARRAY_MEMBER_REFERENCE_CONSUME_INTO_STORAGE,
     XR_ARRAY_MEMBER_REFERENCE_DROP_RELEASE_ON_ERASE_OR_DESTROY, 0},
    {"unshift", 2, 2, XR_ARRAY_MEMBER_RESULT_UNIT, 1, XR_ARRAY_MEMBER_ELEMENT_ACCESS_STORE,
     XR_ARRAY_MEMBER_REFERENCE_UNSUPPORTED, XR_ARRAY_MEMBER_REFERENCE_DROP_NONE, 0},
    {"indexOf", 2, 2, XR_ARRAY_MEMBER_RESULT_INT, 1, XR_ARRAY_MEMBER_ELEMENT_ACCESS_READ,
     XR_ARRAY_MEMBER_REFERENCE_UNSUPPORTED, XR_ARRAY_MEMBER_REFERENCE_DROP_NONE, 0},
    {"contains", 2, 2, XR_ARRAY_MEMBER_RESULT_BOOL, 1, XR_ARRAY_MEMBER_ELEMENT_ACCESS_READ,
     XR_ARRAY_MEMBER_REFERENCE_UNSUPPORTED, XR_ARRAY_MEMBER_REFERENCE_DROP_NONE, 0},
    {"fill", 2, 4, XR_ARRAY_MEMBER_RESULT_RECEIVER, 1, XR_ARRAY_MEMBER_ELEMENT_ACCESS_STORE,
     XR_ARRAY_MEMBER_REFERENCE_CONSUME_INTO_STORAGE,
     XR_ARRAY_MEMBER_REFERENCE_DROP_RELEASE_ON_ERASE_OR_DESTROY, 0},
    {"reverse", 1, 1, XR_ARRAY_MEMBER_RESULT_RECEIVER, 0, XR_ARRAY_MEMBER_ELEMENT_ACCESS_MOVE,
     XR_ARRAY_MEMBER_REFERENCE_PRESERVE, XR_ARRAY_MEMBER_REFERENCE_DROP_NONE, 0},
    {"sort", 1, 1, XR_ARRAY_MEMBER_RESULT_RECEIVER, 0, XR_ARRAY_MEMBER_ELEMENT_ACCESS_MOVE,
     XR_ARRAY_MEMBER_REFERENCE_PRESERVE, XR_ARRAY_MEMBER_REFERENCE_DROP_NONE, 0},
    /* Reads every element and builds a string from them: it stores nothing
     * into the container, so reference-capable elements are fine, and the
     * string it returns is freshly owned rather than a borrow of anything. */
    {"join", 2, 2, XR_ARRAY_MEMBER_RESULT_STRING, 0, XR_ARRAY_MEMBER_ELEMENT_ACCESS_READ,
     XR_ARRAY_MEMBER_REFERENCE_PRESERVE, XR_ARRAY_MEMBER_REFERENCE_DROP_NONE, 1},
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

static inline bool xr_semantic_array_member_string_type_is_exact(const XrSemanticTypeRecord *type) {
    return type && type->kind == XR_KIND_STRING && type->builtin_type == XR_TID_NULL &&
           type->child_count == 0 && type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE && (type->flags & XR_SEM_TYPE_NULLABLE) == 0;
}

/* The clause every non-receiver operand of a member must satisfy.
 *
 * Four layers reconstruct this -- the Xi guard, the semantic judgement, its
 * verifier, and the target builder -- and each used to spell the argument rule
 * itself, which is why teaching the family one member with a string argument
 * meant finding all four. The rule lives here now so the table stays the only
 * place a member is described. */
static inline bool xr_semantic_array_member_argument_is_exact(
    const XrArrayMemberShape *shape, const XrSemanticOperandRecord *argument,
    const XrSemanticTypeRecord *argument_type, uint16_t ordinal, uint32_t element_type_index) {
    if (!shape || !argument || !argument_type || argument->role != XR_SEM_OPERAND_ARGUMENT ||
        argument->parameter != (int16_t) (ordinal - 1) ||
        argument->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        argument->ownership_action != XR_SEM_OPERAND_CONSUME)
        return false;
    if (ordinal == shape->element_operand)
        return argument->type == element_type_index;
    if (shape->string_operand != 0 && ordinal == shape->string_operand)
        return xr_semantic_array_member_string_type_is_exact(argument_type);
    return xr_semantic_array_member_i64_type_is_exact(argument_type);
}

/* Reference elements use the runtime's tagged XrValue lane. Its closed
 * ownership roster is an exact String, a frozen source-class instance, or an
 * exact compiler-owned Array. Each is one ownership root whose release is
 * already defined by the runtime value tag. Keeping this judgement beside the
 * selector lifecycle table prevents four plan layers from narrowing the same
 * language fact independently. */
static inline bool
xr_semantic_array_member_owned_reference_type_is_exact(const XrSemanticPlan *plan,
                                                       const XrSemanticTypeRecord *type) {
    return xr_semantic_tagged_string_type_is_exact(type) ||
           xr_semantic_class_instance_type_source_class(plan, type) != XR_SEMANTIC_INDEX_NONE ||
           xr_semantic_array_type_row_is_exact(type);
}

static inline bool xr_semantic_array_member_reference_contract_is_exact(
    const XrSemanticPlan *plan, const XrArrayMemberShape *shape,
    const XrSemanticOperationRecord *operation, uint32_t element_type_index,
    const XrSemanticTypeRecord *element_type) {
    if (!plan || !shape || !operation || !element_type)
        return false;
    if ((element_type->flags & XR_SEM_TYPE_REFERENCE_CAPABLE) == 0)
        return true;
    if (shape->reference_action == XR_ARRAY_MEMBER_REFERENCE_PRESERVE)
        return shape->element_operand == 0 &&
               (shape->element_access == XR_ARRAY_MEMBER_ELEMENT_ACCESS_READ ||
                shape->element_access == XR_ARRAY_MEMBER_ELEMENT_ACCESS_MOVE) &&
               shape->reference_drop == XR_ARRAY_MEMBER_REFERENCE_DROP_NONE;
    if (shape->reference_action != XR_ARRAY_MEMBER_REFERENCE_CONSUME_INTO_STORAGE ||
        shape->element_access != XR_ARRAY_MEMBER_ELEMENT_ACCESS_STORE ||
        shape->reference_drop != XR_ARRAY_MEMBER_REFERENCE_DROP_RELEASE_ON_ERASE_OR_DESTROY ||
        shape->element_operand == 0 || shape->element_operand >= operation->operand_count)
        return false;
    bool exact_push = strcmp(shape->selector, "push") == 0 && operation->operand_count == 2 &&
                      operation->semantic_immediate == (int64_t) XI_METHOD_SYMBOL_PUSH << 1;
    bool exact_fill = strcmp(shape->selector, "fill") == 0 && operation->operand_count == 4 &&
                      operation->semantic_immediate == (int64_t) XI_METHOD_SYMBOL_FILL << 1;
    /* Filling a range duplicates one input into several slots. The current
     * source-class/String path owns the retain contract for that operation;
     * an Array value is admitted only by push, which moves the one ownership
     * root into exactly one new slot. */
    if ((!exact_push && !exact_fill) ||
        (xr_semantic_array_type_row_is_exact(element_type) && !exact_push) ||
        !xr_semantic_array_member_owned_reference_type_is_exact(plan, element_type))
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    uint32_t semantic_operand = operation->operand_begin + shape->element_operand;
    if (!operands || semantic_operand >= operand_count)
        return false;
    const XrSemanticOperandRecord *element = &operands[semantic_operand];
    return element->type == element_type_index && element->role == XR_SEM_OPERAND_ARGUMENT &&
           element->parameter == (int16_t) (shape->element_operand - 1u) &&
           element->flags == XR_SEM_OPERAND_CALL_CONTRACT &&
           element->ownership_action == XR_SEM_OPERAND_CONSUME;
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
    if (shape->result_shape == XR_ARRAY_MEMBER_RESULT_STRING) {
        /* A freshly built string, owned outright: unlike the scalar results
         * below it is a value the caller has to release, so it states an owned
         * return rather than the call-result placeholder those use. */
        return xr_semantic_array_member_string_type_is_exact(result_type) &&
               operation->result_alias_operand == -1 &&
               operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
               operation->return_provenance == XR_SEM_RETURN_OWNED &&
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
