/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_operand_roles.h - Shared instruction roles for values and logical places
 *
 * KEY CONCEPT:
 *   A projection is a restricted use role, never an owned value payload.
 */
#ifndef XXIR_OPERAND_ROLES_H
#define XXIR_OPERAND_ROLES_H
#include "xxir.h"

typedef enum XrXirPlaceKind {
    XR_XIR_PLACE_NONE, XR_XIR_PLACE_LOCAL, XR_XIR_PLACE_CELL, XR_XIR_PLACE_SLOT,
    XR_XIR_PLACE_FIELD, XR_XIR_PLACE_INDEX
} XrXirPlaceKind;

typedef enum XrXirOperandRole {
    XR_XIR_OPERAND_VALUE, XR_XIR_OPERAND_READ, XR_XIR_OPERAND_WRITE
} XrXirOperandRole;

/* Classification grants neither type validity nor permissions or dominance. */
static inline XrXirPlaceKind xr_xir_place_kind(const XrXirFunction *function, uint32_t value) {
    if (!function || !function->instructions || value < function->parameter_count ||
        value - function->parameter_count >= function->instruction_count) return XR_XIR_PLACE_NONE;
    XrXirOp op = function->instructions[value - function->parameter_count].op;
    if (op == XR_XIR_LOCAL_UNINIT || op == XR_XIR_LOCAL_NEW || op == XR_XIR_SCALAR_LOCAL_NEW || op == XR_XIR_OWNED_LOCAL_NEW)
        return XR_XIR_PLACE_LOCAL;
    if (op == XR_XIR_CELL_PLACE) return XR_XIR_PLACE_CELL;
    if (op == XR_XIR_SLOT_PLACE) return XR_XIR_PLACE_SLOT;
    if (op == XR_XIR_FIELD_PLACE) return XR_XIR_PLACE_FIELD;
    if (op == XR_XIR_INDEX_PLACE) return XR_XIR_PLACE_INDEX;
    return XR_XIR_PLACE_NONE;
}

/* Ordinals describe expanded semantic operands, including ARRAY_SET's table. */
static inline XrXirOperandRole xr_xir_operand_role(XrXirOp op, uint32_t ordinal) {
    if (!ordinal && (op == XR_XIR_ARRAY_GET || op == XR_XIR_ARRAY_LEN || op == XR_XIR_PLACE_READ ||
        op == XR_XIR_FIELD_PLACE || op == XR_XIR_INDEX_PLACE)) return XR_XIR_OPERAND_READ;
    if (!ordinal && (op == XR_XIR_ARRAY_SET || op == XR_XIR_ARRAY_PUSH || op == XR_XIR_STRUCT_SET ||
        op == XR_XIR_PLACE_WRITE)) return XR_XIR_OPERAND_WRITE;
    return XR_XIR_OPERAND_VALUE;
}

static inline bool xr_xir_op_references_function(XrXirOp op) {
    return op == XR_XIR_CALL || op == XR_XIR_INVOKE || op == XR_XIR_FUNCTION_REF || op == XR_XIR_CLEANUP_REGISTER;
}
static inline bool xr_xir_op_uses_operand_table(XrXirOp op) {
    return xr_xir_op_references_function(op) || op == XR_XIR_CALL_REQUIREMENT || op == XR_XIR_CALL_INDIRECT || op == XR_XIR_INVOKE_INDIRECT ||
        op == XR_XIR_PRINT || op == XR_XIR_PHI || op == XR_XIR_ARRAY_NEW || op == XR_XIR_ARRAY_SET ||
        op == XR_XIR_STRUCT_NEW || op == XR_XIR_ENUM_NEW || op == XR_XIR_STRING_INDEX_OF;
}
static inline bool xr_xir_op_uses_type_arguments(XrXirOp op) {
    return xr_xir_op_references_function(op) || op == XR_XIR_CALL_REQUIREMENT;
}
static inline bool xr_xir_op_uses_value_path(const XrXirFunction *function, const XrXirInstruction *op) {
    if (op->op == XR_XIR_PLACE_READ || op->op == XR_XIR_PLACE_WRITE) return true;
    if (op->op != XR_XIR_ARRAY_GET && op->op != XR_XIR_ARRAY_SET && op->op != XR_XIR_ARRAY_PUSH &&
        op->op != XR_XIR_ARRAY_LEN && op->op != XR_XIR_STRUCT_SET) return false;
    uint32_t id = xr_xir_op_uses_operand_table(op->op) ? function->operands[op->args[0]] : op->args[0];
    XrXirPlaceKind kind = xr_xir_place_kind(function, id);
    return kind == XR_XIR_PLACE_FIELD || kind == XR_XIR_PLACE_INDEX;
}
#endif // XXIR_OPERAND_ROLES_H
