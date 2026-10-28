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
#include "xxir_declarations.h"
#include "xxir_types.h"

/* Classification is not permission or publication evidence. */
static inline bool xr_xir_slot_is_unit(const XrXirSlot *slots, uint32_t count,
                                      const XrXirInstruction *op) {
    return slots && op && op->immediate >= 0 && (uint64_t)op->immediate < count &&
        slots[op->immediate].type == XR_XIR_UNIT;
}
static inline uint32_t xr_xir_slot_payload_operands(const XrXirSlot *slots, uint32_t count,
                                                  const XrXirInstruction *op) {
    return xr_xir_slot_is_unit(slots,count,op) ? 0u : 1u;
}

/* Cell<Unit> stores initialization and ownership without an SSA payload. */
static inline bool xr_xir_cell_is_unit(const XrXirTypes *types, XrXirType cell) {
    return xr_xir_type_is_cell(types, cell) && xr_xir_cell_element(types, cell) == XR_XIR_UNIT;
}
static inline uint32_t xr_xir_cell_payload_operands(const XrXirTypes *types,
    XrXirType cell, XrXirOp op) {
    bool unit = xr_xir_cell_is_unit(types, cell);
    return op == XR_XIR_CELL_NEW ? !unit :
        op == XR_XIR_CELL_READ ? 1u : 1u + !unit;
}

/* A present Unit has a tag and an exact zero value, not an SSA payload. */
static inline bool xr_xir_nullable_is_unit(const XrXirTypes *types,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    return node && node->kind==XR_XIR_TYPE_NULLABLE && node->element==XR_XIR_UNIT;
}

typedef enum XrXirPlaceKind {
    XR_XIR_PLACE_NONE, XR_XIR_PLACE_LOCAL, XR_XIR_PLACE_CELL, XR_XIR_PLACE_SLOT,
    XR_XIR_PLACE_FIELD, XR_XIR_PLACE_INDEX, XR_XIR_PLACE_OBJECT
} XrXirPlaceKind;

typedef enum XrXirOperandRole {
    XR_XIR_OPERAND_VALUE, XR_XIR_OPERAND_READ, XR_XIR_OPERAND_WRITE
} XrXirOperandRole;

/* Classification grants neither type validity nor permissions or dominance. */
static inline XrXirPlaceKind xr_xir_place_op_kind(XrXirOp op) {
    if (op == XR_XIR_LOCAL_UNINIT || op == XR_XIR_LOCAL_NEW || op == XR_XIR_SCALAR_LOCAL_NEW || op == XR_XIR_OWNED_LOCAL_NEW)
        return XR_XIR_PLACE_LOCAL;
    if (op == XR_XIR_CELL_PLACE) return XR_XIR_PLACE_CELL;
    if (op == XR_XIR_SLOT_PLACE) return XR_XIR_PLACE_SLOT;
    if (op == XR_XIR_FIELD_PLACE) return XR_XIR_PLACE_FIELD;
    if (op == XR_XIR_INDEX_PLACE) return XR_XIR_PLACE_INDEX;
    if (op == XR_XIR_OBJECT_PLACE) return XR_XIR_PLACE_OBJECT;
    return XR_XIR_PLACE_NONE;
}

static inline XrXirPlaceKind xr_xir_place_kind(const XrXirFunction *function, uint32_t value) {
    if (!function || !function->instructions || value < function->parameter_count ||
        value - function->parameter_count >= function->instruction_count) return XR_XIR_PLACE_NONE;
    return xr_xir_place_op_kind(function->instructions[value - function->parameter_count].op);
}

/* Ordinals describe expanded semantic operands, including ARRAY_SET's table. */
static inline XrXirOperandRole xr_xir_operand_role(XrXirOp op, uint32_t ordinal) {
    if (!ordinal && (op == XR_XIR_MAP_GET || op == XR_XIR_MAP_INDEX || op == XR_XIR_HASH_KEYS ||
        op == XR_XIR_HASH_VALUES || op == XR_XIR_MAP_ENTRIES || op == XR_XIR_HASH_CONTAINS || op == XR_XIR_HASH_LEN ||
        (op >= XR_XIR_SET_UNION && op <= XR_XIR_MAP_VALUE_AT)))
        return XR_XIR_OPERAND_READ;
    if (!ordinal && (op == XR_XIR_MAP_SET || op == XR_XIR_SET_ADD ||
        op == XR_XIR_HASH_DELETE || op == XR_XIR_HASH_CLEAR)) return XR_XIR_OPERAND_WRITE;
    if (!ordinal && (op == XR_XIR_ARRAY_GET || op == XR_XIR_ARRAY_LEN || op == XR_XIR_ARRAY_CAPACITY || op == XR_XIR_PLACE_READ ||
        op == XR_XIR_FIELD_PLACE || op == XR_XIR_INDEX_PLACE)) return XR_XIR_OPERAND_READ;
    if (!ordinal && (op == XR_XIR_ARRAY_SET || op == XR_XIR_ARRAY_PUSH || op == XR_XIR_STRUCT_SET ||
        op == XR_XIR_PLACE_WRITE || op == XR_XIR_CELL_PROJECT)) return XR_XIR_OPERAND_WRITE;
    return XR_XIR_OPERAND_VALUE;
}

static inline bool xr_xir_op_references_function(XrXirOp op) {
    return op == XR_XIR_CALL || op == XR_XIR_INVOKE || op == XR_XIR_FUNCTION_REF ||
        op == XR_XIR_CLEANUP_REGISTER || op == XR_XIR_GO || op == XR_XIR_GENERATOR_NEW;
}
/* The declaration role is retained even when specialization removes every yield. */
static inline bool xr_xir_function_is_generator(const XrXirModule *module,uint32_t function) {
    return module && module->declarations && function<module->function_count &&
        (module->declarations->functions[function].promises & XR_XIR_FUNCTION_GENERATOR)!=0;
}
static inline XrXirType xr_xir_function_completion(const XrXirModule *module,uint32_t function) {
    return xr_xir_function_is_generator(module,function) ? XR_XIR_UNIT : module->functions[function].result;
}
/* Prior module/declaration verification is required; roles do not grant access. */
static inline bool xr_xir_function_has_go_role(const XrXirModule *module, uint32_t function) {
    const XrXirDeclarations *d = module->declarations;
    if (!d || function >= module->function_count) return false;
    const XrXirFunctionIdentity *identity = &d->functions[function];
    return !identity->cleanup_owner && !identity->test_role &&
        function != d->modules[identity->module].initializer;
}
/* The shared cell is reached through an ordinary owned value, never a place. */
static inline bool xr_xir_op_is_atomic(XrXirOp op) {
    return op >= XR_XIR_ATOMIC_NEW && op <= XR_XIR_ATOMIC_TO_STRING;
}
static inline bool xr_xir_op_is_channel(XrXirOp op) {
    return op>=XR_XIR_CHANNEL_NEW && op<=XR_XIR_CHANNEL_RECV_TIMEOUT;
}
static inline uint32_t xr_xir_atomic_required_operands(XrXirOp op) {
    if (op == XR_XIR_ATOMIC_COMPARE_EXCHANGE) return 3;
    if (op == XR_XIR_ATOMIC_STORE || (op >= XR_XIR_ATOMIC_ADD && op <= XR_XIR_ATOMIC_SWAP)) return 2;
    return 1;
}
static inline bool xr_xir_op_uses_operand_table(XrXirOp op) {
    if (op == XR_XIR_CALL_INTRINSIC) return true;
    return xr_xir_op_references_function(op) || op == XR_XIR_CALL_REQUIREMENT || op == XR_XIR_CALL_INDIRECT || op == XR_XIR_INVOKE_INDIRECT ||
        op == XR_XIR_PRINT || op == XR_XIR_PHI || op == XR_XIR_ARRAY_NEW || op == XR_XIR_ARRAY_SET ||
        op == XR_XIR_STRUCT_NEW || op == XR_XIR_ENUM_NEW || op == XR_XIR_CLASS_NEW || op == XR_XIR_STRING_INDEX_OF || op == XR_XIR_STRING_REPLACE ||
        op == XR_XIR_TUPLE_NEW || op == XR_XIR_SLOT_GROUP_INIT || op == XR_XIR_RANGE_CHECK || op==XR_XIR_CHANNEL_TRY_SEND ||
        op==XR_XIR_CHANNEL_SEND || op==XR_XIR_CHANNEL_RECV_OR ||
        op==XR_XIR_CHANNEL_SEND_TIMEOUT || op==XR_XIR_CHANNEL_RECV_TIMEOUT ||
        op == XR_XIR_MAP_NEW || op == XR_XIR_SET_NEW || op == XR_XIR_MAP_SET ||
        op == XR_XIR_VIEW_NEW || op == XR_XIR_VIEW_REBORROW || op == XR_XIR_VIEW_SET || op == XR_XIR_VIEW_CLEANUP ||
        (xr_xir_op_is_atomic(op) && op != XR_XIR_ATOMIC_NEW);
}
/* These are physical owned-slot identities, not SSA reads or place accesses.
 * Their bounds and types require independent verification. */
static inline bool xr_xir_op_uses_owner_slots(XrXirOp op) { return op==XR_XIR_VIEW_CLEANUP; }
static inline bool xr_xir_op_uses_type_arguments(XrXirOp op) {
    return xr_xir_op_references_function(op) || op == XR_XIR_CALL_REQUIREMENT || op == XR_XIR_CALL_DEFAULT || op == XR_XIR_INVOKE_DEFAULT;
}
static inline bool xr_xir_op_uses_value_path(const XrXirFunction *function, const XrXirInstruction *op) {
    if (op->op == XR_XIR_PLACE_READ || op->op == XR_XIR_PLACE_WRITE || op->op == XR_XIR_CELL_PROJECT) return true;
    if (op->op >= XR_XIR_MAP_GET && op->op <= XR_XIR_MAP_VALUE_AT) {
        uint32_t id = xr_xir_op_uses_operand_table(op->op) ? function->operands[op->args[0]] : op->args[0];
        XrXirPlaceKind kind = xr_xir_place_kind(function, id);
        return kind == XR_XIR_PLACE_FIELD || kind == XR_XIR_PLACE_INDEX;
    }
    if (op->op != XR_XIR_ARRAY_GET && op->op != XR_XIR_ARRAY_SET && op->op != XR_XIR_ARRAY_PUSH &&
        op->op != XR_XIR_ARRAY_LEN && op->op != XR_XIR_ARRAY_CAPACITY && op->op != XR_XIR_STRUCT_SET) return false;
    uint32_t id = xr_xir_op_uses_operand_table(op->op) ? function->operands[op->args[0]] : op->args[0];
    XrXirPlaceKind kind = xr_xir_place_kind(function, id);
    /* A direct Cell may be a scoped Class projection. Its operations need
     * the same genuine instruction proof as a longer logical path. */
    return kind == XR_XIR_PLACE_CELL || kind == XR_XIR_PLACE_FIELD || kind == XR_XIR_PLACE_INDEX;
}
#endif // XXIR_OPERAND_ROLES_H
