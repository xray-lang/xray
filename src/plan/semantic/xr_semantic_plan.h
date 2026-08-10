/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_plan.h - Immutable target-neutral semantic plan schema
 *
 * KEY CONCEPT:
 *   This plan is the last semantic authority shared by every executor and
 *   target planner. Records contain stable IDs, indexes, and owned strings;
 *   they never contain compiler pointers or provider spellings.
 */

#ifndef XR_SEMANTIC_PLAN_H
#define XR_SEMANTIC_PLAN_H

#include "xr_semantic_ids.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define XR_SEMANTIC_INDEX_NONE UINT32_MAX

typedef struct XrSemanticPlan XrSemanticPlan;
typedef struct XrOwnershipCertificate XrOwnershipCertificate;

typedef enum XrSemanticConstantKind {
    XR_SEM_CONST_NONE = 0,
    XR_SEM_CONST_NULL,
    XR_SEM_CONST_INT,
    XR_SEM_CONST_FLOAT,
    XR_SEM_CONST_BOOL,
    XR_SEM_CONST_RUNE,
    XR_SEM_CONST_STRING,
    XR_SEM_CONST_ENUM_NAMESPACE,
} XrSemanticConstantKind;

typedef enum XrSemanticReturnProvenance {
    XR_SEM_RETURN_NONE = 0,
    XR_SEM_RETURN_OWNED,
    XR_SEM_RETURN_BORROWED_PARAM,
    XR_SEM_RETURN_BORROWED_STATIC,
} XrSemanticReturnProvenance;

typedef enum XrSemanticOperandOwnershipAction {
    XR_SEM_OPERAND_BORROW = 0,
    XR_SEM_OPERAND_CONSUME = 1,
} XrSemanticOperandOwnershipAction;

typedef enum XrSemanticOperandRole {
    XR_SEM_OPERAND_VALUE = 0,
    XR_SEM_OPERAND_CALLEE,
    XR_SEM_OPERAND_RECEIVER,
    XR_SEM_OPERAND_ARGUMENT,
    XR_SEM_OPERAND_ROLE_COUNT,
} XrSemanticOperandRole;

typedef enum XrSemanticOperandFlag {
    XR_SEM_OPERAND_CALL_CONTRACT = 1u << 0,
    XR_SEM_OPERAND_ADDRESSABLE = 1u << 1,
} XrSemanticOperandFlag;

typedef enum XrSemanticTypeFlag {
    XR_SEM_TYPE_NULLABLE = 1u << 0,
    XR_SEM_TYPE_CONST = 1u << 1,
    XR_SEM_TYPE_VALUE = 1u << 2,
    XR_SEM_TYPE_LITERAL = 1u << 3,
    XR_SEM_TYPE_REFERENCE_CAPABLE = 1u << 4,
    XR_SEM_TYPE_BORROW_VIEW = 1u << 5,
    XR_SEM_TYPE_OWNERSHIP_ROOT = 1u << 6,
} XrSemanticTypeFlag;

typedef enum XrSemanticEdgeKind {
    XR_SEM_EDGE_NORMAL = 0,
    XR_SEM_EDGE_ERROR,
    XR_SEM_EDGE_PANIC,
    XR_SEM_EDGE_CANCEL,
    XR_SEM_EDGE_SUSPEND,
    XR_SEM_EDGE_RESUME,
} XrSemanticEdgeKind;

typedef enum XrSemanticEdgeFlag {
    XR_SEM_EDGE_HANDLER_SCOPE = 1u << 0,
} XrSemanticEdgeFlag;

typedef struct XrSemanticTypeRecord {
    XrStableId id;
    const char *canonical_key;
    uint32_t kind;
    uint32_t child_begin;
    uint16_t child_count;
    uint8_t scalar_rep;
    uint8_t flags;
} XrSemanticTypeRecord;

typedef struct XrSemanticFunctionRecord {
    XrStableId id;
    const char *canonical_key;
    const char *name;
    uint32_t return_type;
    uint32_t parameter_begin;
    uint16_t parameter_count;
    uint16_t child_count;
    uint32_t block_begin;
    uint32_t block_count;
    uint32_t value_begin;
    uint32_t value_count;
    uint32_t semantic_effects;
    uint32_t capability_mask;
    int16_t return_parameter;
    uint8_t return_provenance;
    uint8_t flags;
} XrSemanticFunctionRecord;

typedef struct XrSemanticBlockRecord {
    XrStableId id;
    const char *canonical_key;
    uint32_t function;
    uint32_t operation_begin;
    uint32_t operation_count;
    uint32_t predecessor_begin;
    uint16_t predecessor_count;
    uint16_t kind;
    uint32_t successors[2];
    uint32_t control_value;
    uint32_t source_line;
} XrSemanticBlockRecord;

typedef struct XrSemanticOperationRecord {
    XrStableId id;
    XrStableId allocation_id;
    const char *canonical_key;
    const char *allocation_key;
    uint32_t function;
    uint32_t block;
    uint32_t result_value;
    uint32_t result_type;
    uint32_t operand_begin;
    uint16_t operand_count;
    uint16_t opcode;
    uint32_t metadata_begin;
    uint16_t metadata_count;
    uint8_t auxiliary_kind;
    uint8_t reserved;
    uint32_t effects;
    uint32_t source_line;
    int64_t semantic_immediate;
    uint32_t constant;
    uint32_t evidence[8];
    uint8_t ownership_use;
    uint8_t result_ownership;
    uint8_t transfer_mode;
    uint8_t parameter_mode;
    uint8_t parameter_ownership;
    uint8_t flags;
    int16_t result_alias_operand;
    int16_t return_parameter;
    uint8_t return_provenance;
    uint8_t return_complete;
} XrSemanticOperationRecord;

typedef struct XrSemanticEdgeRecord {
    XrStableId id;
    const char *canonical_key;
    uint32_t function;
    uint32_t from_block;
    uint32_t to_block;
    uint32_t operation;
    uint8_t kind;
    uint8_t flags;
    uint16_t reserved;
} XrSemanticEdgeRecord;

typedef struct XrSemanticConstantRecord {
    uint32_t type;
    uint8_t kind;
    uint8_t reserved[3];
    int64_t integer;
    uint64_t float_bits;
    const char *string;
} XrSemanticConstantRecord;

typedef struct XrSemanticOperandRecord {
    uint32_t value;
    uint32_t type;
    int16_t parameter;
    uint8_t role;
    uint8_t transfer_mode;
    uint8_t ownership_action;
    uint8_t parameter_mode;
    uint8_t access;
    uint8_t origin;
    uint8_t lifetime;
    uint8_t escape;
    uint8_t flags;
} XrSemanticOperandRecord;

XR_FUNC XrSemanticPlan *xr_semantic_plan_retain(XrSemanticPlan *plan);
XR_FUNC void xr_semantic_plan_free(XrSemanticPlan *plan);
XR_FUNC bool xr_semantic_plan_is_frozen(const XrSemanticPlan *plan);
XR_FUNC bool xr_semantic_plan_is_verified(const XrSemanticPlan *plan);
XR_FUNC uint32_t xr_semantic_plan_schema(const XrSemanticPlan *plan);
XR_FUNC XrFingerprint xr_semantic_plan_fingerprint(const XrSemanticPlan *plan);
XR_FUNC XrFingerprint xr_semantic_plan_operation_registry_fingerprint(const XrSemanticPlan *plan);
XR_FUNC size_t xr_semantic_plan_type_count(const XrSemanticPlan *plan);
XR_FUNC size_t xr_semantic_plan_function_count(const XrSemanticPlan *plan);
XR_FUNC size_t xr_semantic_plan_block_count(const XrSemanticPlan *plan);
XR_FUNC size_t xr_semantic_plan_operation_count(const XrSemanticPlan *plan);
XR_FUNC size_t xr_semantic_plan_edge_count(const XrSemanticPlan *plan);
XR_FUNC size_t xr_semantic_plan_constant_count(const XrSemanticPlan *plan);
XR_FUNC const XrSemanticTypeRecord *xr_semantic_plan_type(const XrSemanticPlan *plan,
                                                          uint32_t index);
XR_FUNC const XrSemanticFunctionRecord *xr_semantic_plan_function(const XrSemanticPlan *plan,
                                                                  uint32_t index);
XR_FUNC const XrSemanticBlockRecord *xr_semantic_plan_block(const XrSemanticPlan *plan,
                                                            uint32_t index);
XR_FUNC const XrSemanticOperationRecord *xr_semantic_plan_operation(const XrSemanticPlan *plan,
                                                                    uint32_t index);
XR_FUNC const XrSemanticEdgeRecord *xr_semantic_plan_edge(const XrSemanticPlan *plan,
                                                          uint32_t index);
XR_FUNC const XrSemanticConstantRecord *xr_semantic_plan_constant(const XrSemanticPlan *plan,
                                                                  uint32_t index);
XR_FUNC const uint32_t *xr_semantic_plan_type_children(const XrSemanticPlan *plan, uint32_t *count);
XR_FUNC const uint32_t *xr_semantic_plan_parameters(const XrSemanticPlan *plan, uint32_t *count);
XR_FUNC const uint32_t *xr_semantic_plan_predecessors(const XrSemanticPlan *plan, uint32_t *count);
XR_FUNC const XrSemanticOperandRecord *xr_semantic_plan_operands(const XrSemanticPlan *plan,
                                                                 uint32_t *count);
XR_FUNC const char *const *xr_semantic_plan_metadata(const XrSemanticPlan *plan, uint32_t *count);
XR_FUNC const XrOwnershipCertificate *xr_semantic_plan_ownership(const XrSemanticPlan *plan);
XR_FUNC bool xr_semantic_plan_dump(const XrSemanticPlan *plan, FILE *out);

#endif  // XR_SEMANTIC_PLAN_H
