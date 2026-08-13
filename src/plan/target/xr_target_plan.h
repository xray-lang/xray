/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_plan.h - Immutable backend-neutral target plan schema
 *
 * KEY CONCEPT:
 *   The plan freezes target decisions as dense numeric tables. It contains no
 *   C spelling, linker symbol, analyzer pointer, or backend-private tag.
 */

#ifndef XR_TARGET_PLAN_H
#define XR_TARGET_PLAN_H

#include "xr_target_profile.h"
#include "../semantic/xr_semantic_plan.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct XrTargetPlan XrTargetPlan;

typedef enum XrTargetPlanFamily {
    XR_TARGET_FAMILY_SCALAR = UINT64_C(1) << 0,
    XR_TARGET_FAMILY_AGGREGATE = UINT64_C(1) << 1,
    XR_TARGET_FAMILY_CALL_ADAPTER = UINT64_C(1) << 2,
    XR_TARGET_FAMILY_CLOSURE_STORAGE = UINT64_C(1) << 3,
    XR_TARGET_FAMILY_COROUTINE_STATE_CALL = UINT64_C(1) << 4,
    XR_TARGET_FAMILY_STRING_LITERAL_STORAGE = UINT64_C(1) << 5,
    XR_TARGET_FAMILY_DIRECT_LOCAL_CALLEE_STORAGE = UINT64_C(1) << 6,
    XR_TARGET_FAMILY_CHANNEL_ALLOCATION_STORAGE = UINT64_C(1) << 7,
    XR_TARGET_FAMILY_CHANNEL_RECEIVE_STORAGE = UINT64_C(1) << 8,
    XR_TARGET_FAMILY_DIRECT_LOCAL_GO_CALLEE_STORAGE = UINT64_C(1) << 9,
    XR_TARGET_FAMILY_SOURCE_NAMESPACE_STORAGE = UINT64_C(1) << 10,
    XR_TARGET_FAMILY_STRING_BYTE_SLICE_VIEW_STORAGE = UINT64_C(1) << 11,
    XR_TARGET_FAMILY_DIRECT_LOCAL_UNIT_ENUM_ARGUMENT_STORAGE = UINT64_C(1) << 12,
    XR_TARGET_FAMILY_STRINGBUILDER_APPEND_RUNE_STORAGE = UINT64_C(1) << 13,
    XR_TARGET_FAMILY_STRINGBUILDER_TO_STRING_STORAGE = UINT64_C(1) << 14,
    XR_TARGET_FAMILY_STRINGBUILDER_APPEND_STRING_STORAGE = UINT64_C(1) << 15,
    XR_TARGET_FAMILY_JSON_NAMESPACE_VALUE_STORAGE = UINT64_C(1) << 16,
} XrTargetPlanFamily;

typedef enum XrTargetExecutionFamily {
    XR_TARGET_EXECUTION_SCALAR_I64_STRAIGHT_LINE = UINT64_C(1) << 0,
} XrTargetExecutionFamily;

typedef enum XrTargetInstructionOpcode {
    XR_TARGET_INSTRUCTION_INVALID = 0,
    XR_TARGET_INSTRUCTION_CONST_I64,
    XR_TARGET_INSTRUCTION_COPY_I64,
    XR_TARGET_INSTRUCTION_ADD_WRAP_I64,
    XR_TARGET_INSTRUCTION_SUB_WRAP_I64,
    XR_TARGET_INSTRUCTION_MUL_WRAP_I64,
    XR_TARGET_INSTRUCTION_BAND_I64,
    XR_TARGET_INSTRUCTION_BOR_I64,
    XR_TARGET_INSTRUCTION_BXOR_I64,
    XR_TARGET_INSTRUCTION_NEG_WRAP_I64,
    XR_TARGET_INSTRUCTION_BNOT_I64,
    XR_TARGET_INSTRUCTION_RETURN_I64,
    /* Binds one incoming argument ordinal, carried in the immediate, to the
     * function's parameter slot. It is a definition row, not a computation, so
     * the same single-assignment and use-after-definition proof covers a
     * parameter read without any implicitly live slot. */
    XR_TARGET_INSTRUCTION_PARAM_I64,
    XR_TARGET_INSTRUCTION_COUNT,
} XrTargetInstructionOpcode;

#define XR_TARGET_INSTRUCTION_SLOT_NONE UINT32_MAX

/* Argument ordinals are proved dense through a fixed-width bitmap, so the
 * executable family caps its parameter count instead of allocating. */
#define XR_TARGET_INSTRUCTION_MAX_PARAMETERS 64u

#define XR_TARGET_REQUIRED_FAMILIES                                                         \
    ((uint64_t) (XR_TARGET_FAMILY_SCALAR | XR_TARGET_FAMILY_AGGREGATE |                  \
                 XR_TARGET_FAMILY_CALL_ADAPTER |                                         \
                 XR_TARGET_FAMILY_CLOSURE_STORAGE |                                      \
                 XR_TARGET_FAMILY_COROUTINE_STATE_CALL |                                 \
                 XR_TARGET_FAMILY_STRING_LITERAL_STORAGE |                               \
                 XR_TARGET_FAMILY_DIRECT_LOCAL_CALLEE_STORAGE |                           \
                 XR_TARGET_FAMILY_CHANNEL_ALLOCATION_STORAGE |                            \
                 XR_TARGET_FAMILY_CHANNEL_RECEIVE_STORAGE |                               \
                 XR_TARGET_FAMILY_DIRECT_LOCAL_GO_CALLEE_STORAGE |                         \
                 XR_TARGET_FAMILY_SOURCE_NAMESPACE_STORAGE |                         \
                 XR_TARGET_FAMILY_STRING_BYTE_SLICE_VIEW_STORAGE |                       \
                 XR_TARGET_FAMILY_DIRECT_LOCAL_UNIT_ENUM_ARGUMENT_STORAGE |                    \
                 XR_TARGET_FAMILY_STRINGBUILDER_APPEND_RUNE_STORAGE |                    \
                 XR_TARGET_FAMILY_STRINGBUILDER_TO_STRING_STORAGE |                     \
                 XR_TARGET_FAMILY_STRINGBUILDER_APPEND_STRING_STORAGE |                  \
                 XR_TARGET_FAMILY_JSON_NAMESPACE_VALUE_STORAGE))

typedef enum XrMachineRepKind {
    XR_MACHINE_REP_VOID = 0,
    XR_MACHINE_REP_I1,
    XR_MACHINE_REP_I8,
    XR_MACHINE_REP_U8,
    XR_MACHINE_REP_I16,
    XR_MACHINE_REP_U16,
    XR_MACHINE_REP_I32,
    XR_MACHINE_REP_U32,
    XR_MACHINE_REP_I64,
    XR_MACHINE_REP_U64,
    XR_MACHINE_REP_ISIZE,
    XR_MACHINE_REP_USIZE,
    XR_MACHINE_REP_F32,
    XR_MACHINE_REP_F64,
    XR_MACHINE_REP_RUNE,
    XR_MACHINE_REP_ENUM_ORDINAL,
    XR_MACHINE_REP_OBJECT_REF,
    XR_MACHINE_REP_RAW_PTR,
    XR_MACHINE_REP_CODE_REF,
    XR_MACHINE_REP_DYN_VALUE,
    XR_MACHINE_REP_AGGREGATE,
    XR_MACHINE_REP_VECTOR,
    XR_MACHINE_REP_VIEW,
    XR_MACHINE_REP_COUNT,
} XrMachineRepKind;

typedef enum XrTargetSignedness {
    XR_TARGET_SIGN_NONE = 0,
    XR_TARGET_SIGN_SIGNED,
    XR_TARGET_SIGN_UNSIGNED,
} XrTargetSignedness;

typedef enum XrTargetRootKind {
    XR_TARGET_ROOT_NONE = 0,
    XR_TARGET_ROOT_OBJECT,
    XR_TARGET_ROOT_DYNAMIC,
    XR_TARGET_ROOT_VIEW_OWNER,
} XrTargetRootKind;

typedef enum XrTargetOwnershipClass {
    XR_TARGET_OWNERSHIP_TRIVIAL = 0,
    XR_TARGET_OWNERSHIP_BORROWED,
    XR_TARGET_OWNERSHIP_OWNED,
    XR_TARGET_OWNERSHIP_SHARED,
} XrTargetOwnershipClass;

typedef enum XrTargetNullEncoding {
    XR_TARGET_NULL_NOT_NULLABLE = 0,
    XR_TARGET_NULL_ZERO,
    XR_TARGET_NULL_TAGGED,
} XrTargetNullEncoding;

typedef enum XrTargetLayoutKind {
    XR_TARGET_LAYOUT_SCALAR = 0,
    XR_TARGET_LAYOUT_OBJECT,
    XR_TARGET_LAYOUT_AGGREGATE,
    XR_TARGET_LAYOUT_DYNAMIC,
    XR_TARGET_LAYOUT_VIEW,
} XrTargetLayoutKind;

typedef enum XrTargetExtentKind {
    XR_TARGET_EXTENT_FIXED = 0,
    XR_TARGET_EXTENT_INLINE_TAIL,
    XR_TARGET_EXTENT_EXTERNAL_BUFFER,
    XR_TARGET_EXTENT_MULTI_BUFFER,
    XR_TARGET_EXTENT_PROVIDER_DEFINED,
} XrTargetExtentKind;

typedef enum XrTargetExtentOperandRole {
    XR_TARGET_EXTENT_OPERAND_COUNT = 0,
    XR_TARGET_EXTENT_OPERAND_CAPACITY,
    XR_TARGET_EXTENT_OPERAND_LENGTH,
    XR_TARGET_EXTENT_OPERAND_PROVIDER,
    XR_TARGET_EXTENT_OPERAND_ROLE_COUNT,
} XrTargetExtentOperandRole;

typedef enum XrTargetStorageKind {
    XR_TARGET_STORAGE_STACK = 0,
    XR_TARGET_STORAGE_CALLER,
    XR_TARGET_STORAGE_EXEC_LOCAL,
    XR_TARGET_STORAGE_REGION_LOCAL,
    XR_TARGET_STORAGE_MODULE_STATIC,
    XR_TARGET_STORAGE_CONST_IMMORTAL,
    XR_TARGET_STORAGE_UNIQUE_HEAP,
    XR_TARGET_STORAGE_SYNC_SHARED,
    XR_TARGET_STORAGE_FOREIGN_BORROW,
    XR_TARGET_STORAGE_FOREIGN_OWNED,
} XrTargetStorageKind;

typedef enum XrTargetCallMode {
    XR_TARGET_CALL_MODE_INVALID = 0,
    XR_TARGET_CALL_VALUE,
    XR_TARGET_CALL_REFERENCE,
    XR_TARGET_CALL_CALLER_STORAGE,
} XrTargetCallMode;

typedef enum XrTargetCallOwnership {
    XR_TARGET_CALL_OWNERSHIP_INVALID = 0,
    XR_TARGET_CALL_NONE,
    XR_TARGET_CALL_READ,
    XR_TARGET_CALL_BORROW,
    XR_TARGET_CALL_MOVE,
    XR_TARGET_CALL_CONSUME,
    XR_TARGET_CALL_WRITEBACK,
    XR_TARGET_CALL_RETURN_OWNED,
} XrTargetCallOwnership;

typedef enum XrTargetCallConvention {
    XR_TARGET_CALL_CONVENTION_INVALID = 0,
    XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL,
    XR_TARGET_CALL_CONVENTION_CHANNEL_CLOSE,
    XR_TARGET_CALL_CONVENTION_SOURCE_EXPORT,
    XR_TARGET_CALL_CONVENTION_STRINGBUILDER_CONSTRUCTOR,
    XR_TARGET_CALL_CONVENTION_STRING_BYTE_SLICE_VIEW,
    XR_TARGET_CALL_CONVENTION_STRINGBUILDER_APPEND_RUNE,
    XR_TARGET_CALL_CONVENTION_STRINGBUILDER_TO_STRING,
    XR_TARGET_CALL_CONVENTION_STRINGBUILDER_APPEND_STRING,
    XR_TARGET_CALL_CONVENTION_JSON_NAMESPACE_VALUE,
} XrTargetCallConvention;

/* Target dispatch authority. SOURCE_EXPORT names only the public dependency
 * wrapper proven by an ordered SemanticPlan module set; it never identifies a
 * private implementation or reuses a dependency function index as a local
 * index. CHANNEL_CLOSE names a sealed runtime receiver operation.
 * JSON_NAMESPACE_VALUE names the sealed compiler-owned JSON class namespace
 * member, whose receiver is a reserved builtin global rather than a value. */
typedef enum XrTargetCallTargetKind {
    XR_TARGET_CALL_TARGET_INVALID = 0,
    XR_TARGET_CALL_TARGET_DIRECT_LOCAL = XR_SEM_CALL_TARGET_DIRECT_LOCAL,
    XR_TARGET_CALL_TARGET_CHANNEL_CLOSE,
    XR_TARGET_CALL_TARGET_SOURCE_EXPORT,
    XR_TARGET_CALL_TARGET_STRINGBUILDER_CONSTRUCTOR,
    XR_TARGET_CALL_TARGET_STRING_BYTE_SLICE_VIEW,
    XR_TARGET_CALL_TARGET_STRINGBUILDER_APPEND_RUNE,
    XR_TARGET_CALL_TARGET_STRINGBUILDER_TO_STRING,
    XR_TARGET_CALL_TARGET_STRINGBUILDER_APPEND_STRING,
    XR_TARGET_CALL_TARGET_JSON_NAMESPACE_VALUE,
} XrTargetCallTargetKind;

typedef enum XrTargetCallErrorMode {
    XR_TARGET_CALL_ERROR_MODE_INVALID = 0,
    XR_TARGET_CALL_NO_CALL_OWNED_CHANNEL,
} XrTargetCallErrorMode;

typedef enum XrTargetCleanupAction {
    XR_TARGET_CLEANUP_RELEASE = 0,
    XR_TARGET_CLEANUP_DESTROY,
    XR_TARGET_CLEANUP_UNPIN,
    XR_TARGET_CLEANUP_PROVIDER,
} XrTargetCleanupAction;

typedef enum XrTargetAdapterKind {
    XR_TARGET_ADAPTER_INVALID = 0,
    XR_TARGET_ADAPTER_BOX_DYNAMIC,
    XR_TARGET_ADAPTER_UNBOX_DYNAMIC,
    XR_TARGET_ADAPTER_FFI,
    XR_TARGET_ADAPTER_HOSTED,
} XrTargetAdapterKind;

typedef enum XrTargetAdapterRole {
    XR_TARGET_ADAPTER_ROLE_INVALID = 0,
    XR_TARGET_ADAPTER_ARGUMENT,
    XR_TARGET_ADAPTER_RESULT,
    XR_TARGET_ADAPTER_ERROR,
    XR_TARGET_ADAPTER_CALLER_STORAGE,
} XrTargetAdapterRole;

typedef enum XrTargetExtentFlag {
    XR_TARGET_EXTENT_ZERO = 1u << 0,
    XR_TARGET_EXTENT_ACCOUNT = 1u << 1,
    XR_TARGET_EXTENT_CLONE = 1u << 2,
    XR_TARGET_EXTENT_TEARDOWN = 1u << 3,
    XR_TARGET_EXTENT_SIZED_DEALLOC = 1u << 4,
} XrTargetExtentFlag;

typedef enum XrTargetStorageFlag {
    XR_TARGET_STORAGE_ESCAPES = 1u << 0,
    XR_TARGET_STORAGE_PUBLISHED = 1u << 1,
    XR_TARGET_STORAGE_HAS_HEADER = 1u << 2,
    XR_TARGET_STORAGE_THREAD_SAFE = 1u << 3,
    XR_TARGET_STORAGE_DETACH = 1u << 4,
    XR_TARGET_STORAGE_TRANSFER = 1u << 5,
} XrTargetStorageFlag;

typedef enum XrTargetCallFlag {
    XR_TARGET_CALL_ERROR = 1u << 0,
    XR_TARGET_CALL_SUSPEND = 1u << 1,
    XR_TARGET_CALL_ENVIRONMENT = 1u << 2,
    XR_TARGET_CALL_GENERATION = 1u << 3,
    XR_TARGET_CALL_TAIL = 1u << 4,
} XrTargetCallFlag;

typedef enum XrTargetCallArgumentFlag {
    XR_TARGET_CALL_ARGUMENT_ADDRESSABLE = 1u << 0,
} XrTargetCallArgumentFlag;

typedef enum XrTargetRootMapFlag {
    XR_TARGET_ROOT_SAFEPOINT = 1u << 0,
    XR_TARGET_ROOT_SUSPEND = 1u << 1,
    XR_TARGET_ROOT_ERROR = 1u << 2,
    XR_TARGET_ROOT_CANCEL = 1u << 3,
    XR_TARGET_ROOT_EXIT = 1u << 4,
} XrTargetRootMapFlag;

typedef enum XrTargetCleanupFlag {
    XR_TARGET_CLEANUP_ERROR = 1u << 0,
    XR_TARGET_CLEANUP_CANCEL = 1u << 1,
    XR_TARGET_CLEANUP_EXIT = 1u << 2,
} XrTargetCleanupFlag;

typedef enum XrTargetAdapterFlag {
    XR_TARGET_ADAPTER_BLOCKING = 1u << 0,
    XR_TARGET_ADAPTER_WRITEBACK = 1u << 1,
} XrTargetAdapterFlag;

typedef enum XrTargetCapabilityFlag {
    XR_TARGET_CAPABILITY_REQUIRED = 1u << 0,
} XrTargetCapabilityFlag;

typedef enum XrTargetSlotRole {
    XR_TARGET_SLOT_ROLE_INVALID = 0,
    XR_TARGET_SLOT_PARAMETER,
    XR_TARGET_SLOT_LOCAL,
    XR_TARGET_SLOT_PHI,
    XR_TARGET_SLOT_TEMPORARY,
    XR_TARGET_SLOT_RETURN,
    XR_TARGET_SLOT_CALLER_STORAGE,
    XR_TARGET_SLOT_LOGICAL_COROUTINE,
    XR_TARGET_SLOT_ROLE_COUNT,
} XrTargetSlotRole;

typedef enum XrTargetCoroutineStateFlag {
    XR_TARGET_COROUTINE_DIRECT_CHILD = 1u << 0,
    XR_TARGET_COROUTINE_RESULT_SLOT_BOUND = 1u << 1,
    XR_TARGET_COROUTINE_SOURCE_CHILD = 1u << 2,
} XrTargetCoroutineStateFlag;

typedef struct XrTargetMachineRepRecord {
    uint32_t id;
    uint16_t kind;
    uint16_t register_bits;
    uint32_t memory_size;
    uint16_t memory_align;
    uint8_t signedness;
    uint8_t root_kind;
    uint8_t ownership;
    uint8_t null_encoding;
    uint32_t detail;
    uint16_t lane_count;
    uint16_t reserved;
    uint64_t legal_conversion_mask[4];
} XrTargetMachineRepRecord;

typedef struct XrTargetValueRepRecord {
    uint32_t semantic_value;
    uint16_t register_rep;
    uint16_t memory_rep;
    uint32_t slot;
} XrTargetValueRepRecord;

typedef struct XrTargetExtentRecord {
    uint32_t id;
    uint8_t kind;
    uint8_t operand_count;
    uint16_t alignment;
    uint32_t element_layout;
    uint32_t stride;
    uint32_t provider;
    uint32_t flags;
} XrTargetExtentRecord;

typedef struct XrTargetLayoutRecord {
    uint32_t id;
    uint32_t semantic_type;
    uint8_t kind;
    uint8_t reserved;
    uint16_t align;
    uint32_t fixed_prefix_size;
    uint32_t extent;
    uint32_t field_begin;
    uint16_t field_count;
    uint16_t root_field_count;
    XrStableId destructor;
    XrStableId clone;
    XrStableId equality_hash;
    XrFingerprint fingerprint;
} XrTargetLayoutRecord;

typedef struct XrTargetFieldRecord {
    uint32_t layout;
    uint32_t semantic_field;
    uint32_t offset;
    uint32_t size;
    uint16_t align;
    uint16_t memory_rep;
    uint8_t root_kind;
    uint8_t flags;
    uint16_t reserved;
} XrTargetFieldRecord;

typedef struct XrTargetStorageRecord {
    uint32_t id;
    uint8_t kind;
    uint8_t ownership;
    uint16_t flags;
    XrStableId domain;
    XrStableId destructor;
} XrTargetStorageRecord;

typedef struct XrTargetAllocationRecord {
    uint32_t id;
    uint32_t semantic_operation;
    uint32_t layout;
    uint32_t storage;
    uint32_t operand_begin;
    uint16_t operand_count;
    uint16_t flags;
} XrTargetAllocationRecord;

typedef struct XrTargetExtentOperandRecord {
    uint32_t allocation;
    uint32_t semantic_value;
    uint8_t ordinal;
    uint8_t role;
    uint16_t reserved;
} XrTargetExtentOperandRecord;

typedef struct XrTargetFunctionRecord {
    uint32_t id;
    uint32_t semantic_function;
    uint32_t slot_begin;
    uint32_t slot_count;
    uint32_t frame_size;
    uint16_t frame_align;
    uint16_t reserved;
    uint32_t root_begin;
    uint32_t root_count;
    uint32_t cleanup_begin;
    uint32_t cleanup_count;
    uint32_t coroutine_begin;
    uint32_t coroutine_count;
} XrTargetFunctionRecord;

typedef struct XrTargetSlotRecord {
    XrStableId identity;
    uint32_t id;
    uint32_t function;
    uint32_t semantic_value;
    uint32_t semantic_operation;
    uint32_t logical_slot;
    uint32_t offset;
    uint32_t size;
    uint16_t align;
    uint16_t register_rep;
    uint16_t memory_rep;
    uint8_t role;
    uint8_t root_kind;
    uint8_t ownership;
    uint8_t reserved;
    uint32_t debug_variable;
} XrTargetSlotRecord;

typedef struct XrTargetInstructionRecord {
    uint32_t id;
    uint32_t function;
    uint32_t result_slot;
    uint32_t operand_slots[2];
    uint64_t immediate_bits;
    uint8_t opcode;
    uint8_t operand_count;
    uint16_t reserved;
} XrTargetInstructionRecord;

typedef struct XrTargetCallArgumentRecord {
    XrStableId identity;
    uint32_t call;
    uint32_t semantic_operand;
    uint32_t semantic_value;
    uint32_t callee_parameter;
    uint32_t caller_slot;
    uint32_t callee_slot;
    uint16_t register_rep;
    uint16_t memory_rep;
    uint16_t ordinal;
    uint8_t mode;
    uint8_t ownership;
    uint8_t transfer_mode;
    uint8_t flags;
} XrTargetCallArgumentRecord;

typedef struct XrTargetCallRecord {
    XrStableId identity;
    uint32_t id;
    uint32_t semantic_call_target;
    uint32_t semantic_operation;
    uint32_t caller_function;
    uint32_t callee_function;
    uint32_t source_dependency;
    uint32_t source_export;
    XrStableId source_export_identity;
    XrStableId source_callee_identity;
    uint32_t result_value;
    uint32_t result_slot;
    uint32_t caller_storage_slot;
    uint32_t error_slot;
    uint32_t argument_begin;
    uint32_t adapter_begin;
    uint16_t result_register_rep;
    uint16_t result_memory_rep;
    uint16_t error_register_rep;
    uint16_t error_memory_rep;
    uint16_t argument_count;
    uint16_t adapter_count;
    uint16_t native_abi;
    uint16_t flags;
    uint8_t calling_convention;
    uint8_t target_kind;
    uint8_t result_mode;
    uint8_t result_ownership;
    uint8_t error_mode;
    uint8_t reserved8;
    XrFingerprint fingerprint;
} XrTargetCallRecord;

typedef struct XrTargetRootMapRecord {
    uint32_t id;
    uint32_t function;
    uint32_t semantic_operation;
    uint32_t slot_begin;
    uint16_t slot_count;
    uint16_t flags;
} XrTargetRootMapRecord;

typedef struct XrTargetCleanupRecord {
    uint32_t id;
    uint32_t function;
    uint32_t semantic_operation;
    uint32_t slot;
    uint8_t action;
    uint8_t flags;
    uint16_t provider;
} XrTargetCleanupRecord;

typedef struct XrTargetAdapterRecord {
    XrStableId identity;
    uint32_t id;
    uint32_t call;
    uint32_t input_rep;
    uint32_t output_rep;
    uint32_t layout;
    uint16_t ordinal;
    uint16_t flags;
    uint8_t role;
    uint8_t kind;
    uint8_t ownership;
    uint8_t reserved;
} XrTargetAdapterRecord;

typedef struct XrTargetCapabilityRecord {
    uint32_t id;
    uint32_t capability;
    uint16_t provider;
    uint16_t flags;
} XrTargetCapabilityRecord;

typedef struct XrTargetCoroutineStateRecord {
    uint32_t id;
    uint32_t function;
    uint32_t semantic_entity;
    uint32_t semantic_operation;
    uint32_t logical_state;
    uint32_t suspend_block;
    uint32_t resume_block;
    uint32_t resume_predecessor;
    uint32_t direct_call;
    uint32_t result_slot;
    uint16_t resume_predecessor_ordinal;
    uint16_t flags;
} XrTargetCoroutineStateRecord;

XR_FUNC XrTargetPlan *xr_target_plan_retain(XrTargetPlan *plan);
XR_FUNC void xr_target_plan_free(XrTargetPlan *plan);
XR_FUNC bool xr_target_plan_is_frozen(const XrTargetPlan *plan);
XR_FUNC bool xr_target_plan_is_verified(const XrTargetPlan *plan);
XR_FUNC uint32_t xr_target_plan_schema_version(const XrTargetPlan *plan);
XR_FUNC XrFingerprint xr_target_plan_fingerprint(const XrTargetPlan *plan);
XR_FUNC XrFingerprint xr_target_plan_semantic_fingerprint(const XrTargetPlan *plan);
XR_FUNC bool xr_target_plan_fingerprint_is_intact(const XrTargetPlan *plan);
XR_FUNC uint64_t xr_target_plan_completed_family_mask(const XrTargetPlan *plan);
XR_FUNC const XrSemanticPlan *xr_target_plan_semantic_plan(const XrTargetPlan *plan);
XR_FUNC const XrTargetProfile *xr_target_plan_profile(const XrTargetPlan *plan);
XR_FUNC const XrTargetMachineRepRecord *xr_target_plan_machine_rep(const XrTargetPlan *plan,
                                                                    uint16_t rep);
XR_FUNC const XrTargetValueRepRecord *xr_target_plan_value_rep(const XrTargetPlan *plan,
                                                                 uint32_t semantic_value);
XR_FUNC uint64_t xr_target_plan_function_execution_family_mask(
    const XrTargetPlan *plan, uint32_t function);
XR_FUNC const XrTargetInstructionRecord *xr_target_plan_function_instructions(
    const XrTargetPlan *plan, uint32_t function, uint32_t *count);

#define XR_TARGET_TABLE_ACCESSOR(name, type)                                                       \
    XR_FUNC const type *xr_target_plan_##name(const XrTargetPlan *plan, uint32_t *count)
XR_TARGET_TABLE_ACCESSOR(machine_reps, XrTargetMachineRepRecord);
XR_TARGET_TABLE_ACCESSOR(value_reps, XrTargetValueRepRecord);
XR_TARGET_TABLE_ACCESSOR(extents, XrTargetExtentRecord);
XR_TARGET_TABLE_ACCESSOR(layouts, XrTargetLayoutRecord);
XR_TARGET_TABLE_ACCESSOR(fields, XrTargetFieldRecord);
XR_TARGET_TABLE_ACCESSOR(storage, XrTargetStorageRecord);
XR_TARGET_TABLE_ACCESSOR(allocations, XrTargetAllocationRecord);
XR_TARGET_TABLE_ACCESSOR(extent_operands, XrTargetExtentOperandRecord);
XR_TARGET_TABLE_ACCESSOR(functions, XrTargetFunctionRecord);
XR_TARGET_TABLE_ACCESSOR(slots, XrTargetSlotRecord);
XR_TARGET_TABLE_ACCESSOR(instructions, XrTargetInstructionRecord);
XR_TARGET_TABLE_ACCESSOR(calls, XrTargetCallRecord);
XR_TARGET_TABLE_ACCESSOR(call_arguments, XrTargetCallArgumentRecord);
XR_TARGET_TABLE_ACCESSOR(root_maps, XrTargetRootMapRecord);
XR_TARGET_TABLE_ACCESSOR(root_slots, uint32_t);
XR_TARGET_TABLE_ACCESSOR(cleanups, XrTargetCleanupRecord);
XR_TARGET_TABLE_ACCESSOR(adapters, XrTargetAdapterRecord);
XR_TARGET_TABLE_ACCESSOR(capabilities, XrTargetCapabilityRecord);
XR_TARGET_TABLE_ACCESSOR(coroutines, XrTargetCoroutineStateRecord);
#undef XR_TARGET_TABLE_ACCESSOR

#endif  // XR_TARGET_PLAN_H
