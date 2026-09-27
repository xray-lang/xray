/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_array_helpers.h - Shared instruction roles for logical Array access
 *
 * KEY CONCEPT:
 *   A projection is a restricted use role, never an owned value payload.
 */
#ifndef XXIR_ARRAY_HELPERS_H
#define XXIR_ARRAY_HELPERS_H
#include "xxir.h"

typedef enum XrXirPlaceKind {
    XR_XIR_PLACE_NONE, XR_XIR_PLACE_LOCAL, XR_XIR_PLACE_CELL, XR_XIR_PLACE_SLOT
} XrXirPlaceKind;

typedef enum XrXirArrayOperandRole {
    XR_XIR_ARRAY_OPERAND_VALUE, XR_XIR_ARRAY_OPERAND_READ, XR_XIR_ARRAY_OPERAND_WRITE
} XrXirArrayOperandRole;

/* Classification grants neither type validity nor permissions or dominance. */
static inline XrXirPlaceKind xr_xir_place_kind(const XrXirFunction *function, uint32_t value) {
    if (!function || !function->instructions || value < function->parameter_count ||
        value - function->parameter_count >= function->instruction_count) return XR_XIR_PLACE_NONE;
    XrXirOp op = function->instructions[value - function->parameter_count].op;
    if (op == XR_XIR_LOCAL_NEW || op == XR_XIR_SCALAR_LOCAL_NEW || op == XR_XIR_OWNED_LOCAL_NEW)
        return XR_XIR_PLACE_LOCAL;
    if (op == XR_XIR_CELL_PLACE) return XR_XIR_PLACE_CELL;
    if (op == XR_XIR_SLOT_PLACE) return XR_XIR_PLACE_SLOT;
    return XR_XIR_PLACE_NONE;
}

/* Ordinals describe expanded semantic operands, including ARRAY_SET's table. */
static inline XrXirArrayOperandRole xr_xir_array_operand_role(XrXirOp op, uint32_t ordinal) {
    if (!ordinal && (op == XR_XIR_ARRAY_GET || op == XR_XIR_ARRAY_LEN)) return XR_XIR_ARRAY_OPERAND_READ;
    if (!ordinal && (op == XR_XIR_ARRAY_SET || op == XR_XIR_ARRAY_PUSH)) return XR_XIR_ARRAY_OPERAND_WRITE;
    return XR_XIR_ARRAY_OPERAND_VALUE;
}

static inline bool xr_xir_op_uses_operand_table(XrXirOp op) {
    return op == XR_XIR_CALL || op == XR_XIR_FUNCTION_REF || op == XR_XIR_CALL_INDIRECT ||
        op == XR_XIR_PRINT || op == XR_XIR_PHI || op == XR_XIR_ARRAY_NEW || op == XR_XIR_ARRAY_SET;
}
#endif // XXIR_ARRAY_HELPERS_H
