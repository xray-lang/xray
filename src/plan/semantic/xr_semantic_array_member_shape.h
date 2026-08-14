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
#include <stdint.h>
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
} XrArrayMemberShape;

/* Operand 0 is always the receiver, so element_operand 0 means the member
 * takes no element of its own. */
static const XrArrayMemberShape xr_array_member_shapes[] = {
    {"push", 2, 2, XR_ARRAY_MEMBER_RESULT_UNIT, 1},
    {"unshift", 2, 2, XR_ARRAY_MEMBER_RESULT_UNIT, 1},
    {"indexOf", 2, 2, XR_ARRAY_MEMBER_RESULT_INT, 1},
    {"contains", 2, 2, XR_ARRAY_MEMBER_RESULT_BOOL, 1},
    {"fill", 2, 4, XR_ARRAY_MEMBER_RESULT_RECEIVER, 1},
    {"reverse", 1, 1, XR_ARRAY_MEMBER_RESULT_RECEIVER, 0},
    {"sort", 1, 1, XR_ARRAY_MEMBER_RESULT_RECEIVER, 0},
};

static inline const XrArrayMemberShape *xr_array_member_shape(const char *selector,
                                                              uint16_t operand_count) {
    if (!selector)
        return NULL;
    for (size_t i = 0; i < sizeof(xr_array_member_shapes) / sizeof(xr_array_member_shapes[0]); i++) {
        const XrArrayMemberShape *shape = &xr_array_member_shapes[i];
        if (strcmp(shape->selector, selector) == 0 && operand_count >= shape->min_operands &&
            operand_count <= shape->max_operands)
            return shape;
    }
    return NULL;
}

#endif  // XR_SEMANTIC_ARRAY_MEMBER_SHAPE_H
