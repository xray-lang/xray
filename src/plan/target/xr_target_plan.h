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
    XR_TARGET_CALL_VALUE = 0,
    XR_TARGET_CALL_REFERENCE,
    XR_TARGET_CALL_CALLER_STORAGE,
} XrTargetCallMode;

typedef enum XrTargetCallOwnership {
    XR_TARGET_CALL_READ = 0,
    XR_TARGET_CALL_BORROW,
    XR_TARGET_CALL_MOVE,
    XR_TARGET_CALL_CONSUME,
    XR_TARGET_CALL_WRITEBACK,
} XrTargetCallOwnership;

typedef enum XrTargetCleanupAction {
    XR_TARGET_CLEANUP_RELEASE = 0,
    XR_TARGET_CLEANUP_DESTROY,
    XR_TARGET_CLEANUP_UNPIN,
    XR_TARGET_CLEANUP_PROVIDER,
} XrTargetCleanupAction;

typedef enum XrTargetAdapterKind {
    XR_TARGET_ADAPTER_BOX_DYNAMIC = 0,
    XR_TARGET_ADAPTER_UNBOX_DYNAMIC,
    XR_TARGET_ADAPTER_FFI,
    XR_TARGET_ADAPTER_HOSTED,
} XrTargetAdapterKind;

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
} XrTargetCallFlag;

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
    uint32_t root_begin;
    uint32_t root_count;
    uint32_t cleanup_begin;
    uint32_t cleanup_count;
} XrTargetFunctionRecord;

typedef struct XrTargetSlotRecord {
    uint32_t id;
    uint32_t function;
    uint32_t offset;
    uint32_t size;
    uint16_t align;
    uint16_t register_rep;
    uint16_t memory_rep;
    uint8_t root_kind;
    uint8_t ownership;
    uint32_t debug_variable;
} XrTargetSlotRecord;

typedef struct XrTargetCallArgumentRecord {
    uint16_t register_rep;
    uint16_t memory_rep;
    uint8_t mode;
    uint8_t ownership;
    uint16_t flags;
} XrTargetCallArgumentRecord;

typedef struct XrTargetCallRecord {
    uint32_t id;
    uint32_t semantic_operation;
    uint32_t callee_function;
    uint16_t result_register_rep;
    uint16_t result_memory_rep;
    uint8_t result_mode;
    uint8_t result_ownership;
    uint16_t flags;
    uint32_t argument_begin;
    uint16_t argument_count;
    uint32_t adapter;
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
    uint32_t id;
    uint8_t kind;
    uint8_t ownership;
    uint16_t flags;
    uint32_t input_rep;
    uint32_t output_rep;
    uint32_t layout;
} XrTargetAdapterRecord;

typedef struct XrTargetCapabilityRecord {
    uint32_t id;
    uint32_t semantic_capability;
    uint16_t provider;
    uint16_t flags;
} XrTargetCapabilityRecord;

typedef struct XrTargetCoroutineStateRecord {
    uint32_t id;
    uint32_t function;
    uint32_t logical_state;
    uint32_t target_state;
    uint32_t root_map;
    uint32_t cleanup;
    uint32_t normal_state;
    uint32_t error_state;
    uint32_t cancel_state;
    uint32_t drop_state;
} XrTargetCoroutineStateRecord;

/*
 * In-memory planning boundary only.  This foundation deliberately defines no
 * serialized .xtp artifact; persistence requires a separately versioned,
 * independently verified format contract.
 */
typedef struct XrTargetPlanDraft {
    const XrSemanticPlan *semantic_plan;
    XrTargetProfile *profile;
    const XrTargetMachineRepRecord *machine_reps;
    uint32_t machine_reps_count;
    const XrTargetValueRepRecord *value_reps;
    uint32_t value_reps_count;
    const XrTargetExtentRecord *extents;
    uint32_t extents_count;
    const XrTargetLayoutRecord *layouts;
    uint32_t layouts_count;
    const XrTargetFieldRecord *fields;
    uint32_t fields_count;
    const XrTargetStorageRecord *storage;
    uint32_t storage_count;
    const XrTargetAllocationRecord *allocations;
    uint32_t allocations_count;
    const XrTargetExtentOperandRecord *extent_operands;
    uint32_t extent_operands_count;
    const XrTargetFunctionRecord *functions;
    uint32_t functions_count;
    const XrTargetSlotRecord *slots;
    uint32_t slots_count;
    const XrTargetCallRecord *calls;
    uint32_t calls_count;
    const XrTargetCallArgumentRecord *call_arguments;
    uint32_t call_arguments_count;
    const XrTargetRootMapRecord *root_maps;
    uint32_t root_maps_count;
    const uint32_t *root_slots;
    uint32_t root_slots_count;
    const XrTargetCleanupRecord *cleanups;
    uint32_t cleanups_count;
    const XrTargetAdapterRecord *adapters;
    uint32_t adapters_count;
    const XrTargetCapabilityRecord *capabilities;
    uint32_t capabilities_count;
    const XrTargetCoroutineStateRecord *coroutines;
    uint32_t coroutines_count;
} XrTargetPlanDraft;

XR_FUNC bool xr_target_plan_freeze(const XrTargetPlanDraft *draft, XrTargetPlan **out,
                                   char *error, size_t error_size);
XR_FUNC XrTargetPlan *xr_target_plan_retain(XrTargetPlan *plan);
XR_FUNC void xr_target_plan_free(XrTargetPlan *plan);
XR_FUNC bool xr_target_plan_is_frozen(const XrTargetPlan *plan);
XR_FUNC bool xr_target_plan_is_verified(const XrTargetPlan *plan);
XR_FUNC uint32_t xr_target_plan_schema_version(const XrTargetPlan *plan);
XR_FUNC XrFingerprint xr_target_plan_fingerprint(const XrTargetPlan *plan);
XR_FUNC XrFingerprint xr_target_plan_semantic_fingerprint(const XrTargetPlan *plan);
XR_FUNC const XrSemanticPlan *xr_target_plan_semantic_plan(const XrTargetPlan *plan);
XR_FUNC const XrTargetProfile *xr_target_plan_profile(const XrTargetPlan *plan);
XR_FUNC const XrTargetMachineRepRecord *xr_target_plan_machine_rep(const XrTargetPlan *plan,
                                                                    uint16_t rep);
XR_FUNC const XrTargetValueRepRecord *xr_target_plan_value_rep(const XrTargetPlan *plan,
                                                                 uint32_t semantic_value);

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
