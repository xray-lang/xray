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

#include "../../base/xchecks.h"
#include "xr_target_instruction_gen.h"
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
    XR_TARGET_FAMILY_SOURCE_IMPORT_STORAGE = UINT64_C(1) << 10,
    XR_TARGET_FAMILY_STRING_BYTE_SLICE_VIEW_STORAGE = UINT64_C(1) << 11,
    XR_TARGET_FAMILY_DIRECT_LOCAL_UNIT_ENUM_ARGUMENT_STORAGE = UINT64_C(1) << 12,
    XR_TARGET_FAMILY_STRINGBUILDER_APPEND_RUNE_STORAGE = UINT64_C(1) << 13,
    XR_TARGET_FAMILY_STRINGBUILDER_TO_STRING_STORAGE = UINT64_C(1) << 14,
    XR_TARGET_FAMILY_STRINGBUILDER_APPEND_STRING_STORAGE = UINT64_C(1) << 15,
    XR_TARGET_FAMILY_JSON_NAMESPACE_VALUE_STORAGE = UINT64_C(1) << 16,
    XR_TARGET_FAMILY_DIRECT_LOCAL_STRING_BOUNDARY_STORAGE = UINT64_C(1) << 17,
    XR_TARGET_FAMILY_ARRAY_ALLOCATION_STORAGE = UINT64_C(1) << 18,
    XR_TARGET_FAMILY_NATIVE_MODULE_NAMESPACE_STORAGE = UINT64_C(1) << 19,
    XR_TARGET_FAMILY_NULLABLE_SCALAR_STORAGE = UINT64_C(1) << 20,
    XR_TARGET_FAMILY_ARRAY_MEMBER_RESULT_STORAGE = UINT64_C(1) << 21,
    XR_TARGET_FAMILY_SOURCE_CLASS_OBJECT_STORAGE = UINT64_C(1) << 22,
    XR_TARGET_FAMILY_SOURCE_CLASS_INSTANCE_STORAGE = UINT64_C(1) << 23,
    XR_TARGET_FAMILY_SOURCE_CLASS_RECEIVER_STORAGE = UINT64_C(1) << 24,
    XR_TARGET_FAMILY_STRING_CONCAT_RESULT_STORAGE = UINT64_C(1) << 25,
    XR_TARGET_FAMILY_SOURCE_CLASS_METHOD_RECEIVER_STORAGE = UINT64_C(1) << 26,
    XR_TARGET_FAMILY_SOURCE_CLASS_ARGUMENT_STORAGE = UINT64_C(1) << 27,
    XR_TARGET_FAMILY_DIRECT_LOCAL_GO_TASK_RESULT_STORAGE = UINT64_C(1) << 28,
    XR_TARGET_FAMILY_PANIC_CATCH_STORAGE = UINT64_C(1) << 29,
    XR_TARGET_FAMILY_ADT_ENUM_STORAGE = UINT64_C(1) << 30,
} XrTargetPlanFamily;

/* MSVC fixes a C enum to a signed 32-bit carrier before considering this
 * value, which would turn bit 31 into -1 when it participates in the required
 * family closure. Keep the next family as an explicitly unsigned mask. */
#define XR_TARGET_FAMILY_ARRAY_INTRINSIC_STORAGE (UINT64_C(1) << 31)
#define XR_TARGET_FAMILY_STRING_RUNES_RESULT_STORAGE (UINT64_C(1) << 32)
#define XR_TARGET_FAMILY_DIRECT_LOCAL_ARRAY_REF_ARGUMENT_STORAGE (UINT64_C(1) << 33)
#define XR_TARGET_FAMILY_STRING_SLICE_RANGE_RESULT_STORAGE (UINT64_C(1) << 34)
#define XR_TARGET_FAMILY_DYNAMIC_ENTRY_EXPECTATION (UINT64_C(1) << 35)
#define XR_TARGET_FAMILY_ARRAY_HOF_RESULT_STORAGE (UINT64_C(1) << 36)
#define XR_TARGET_FAMILY_DIRECT_LOCAL_AGGREGATE_RESULT_STORAGE (UINT64_C(1) << 37)

typedef enum XrTargetExecutionFamily {
    /* One closed signed-i64 program per function. It is not a straight line:
     * the family admits several basic blocks joined by unconditional jumps and
     * conditional branches, and its control flow is proved from the rows
     * alone rather than assumed from their order. */
    XR_TARGET_EXECUTION_SCALAR_I64_CLOSED = UINT64_C(1) << 0,
    /* Same exact scalar row family, with one or more SOURCE_EXPORT entry
     * calls whose persistent expectations are resolved by the runtime. */
    XR_TARGET_EXECUTION_SCALAR_I64_DYNAMIC = UINT64_C(1) << 1,
    /* A persistent packed scalar frame whose only suspension boundary is an
     * exact coroutine-state row. Resume enters the frozen continuation block;
     * no legacy interpreter stack is retained. */
    XR_TARGET_EXECUTION_SCALAR_I64_COROUTINE = UINT64_C(1) << 2,
} XrTargetExecutionFamily;

#define XR_TARGET_INSTRUCTION_SLOT_NONE UINT32_MAX
#define XR_TARGET_INSTRUCTION_SUSPEND_PACK(state, resume)                                          \
    (((uint64_t) (resume) << 32u) | (uint64_t) (state))
#define XR_TARGET_INSTRUCTION_SUSPEND_STATE(immediate)                                             \
    ((uint32_t) ((immediate) & UINT64_C(0xffffffff)))
#define XR_TARGET_INSTRUCTION_SUSPEND_RESUME(immediate) ((uint32_t) ((immediate) >> 32u))

/* Argument ordinals are proved dense through a fixed-width bitmap, so the
 * executable family caps its parameter count instead of allocating. */
#define XR_TARGET_INSTRUCTION_MAX_PARAMETERS 64u

/* The definite-assignment proof holds one slot bitmap per block, so the
 * executable family caps blocks instead of letting the proof allocate without
 * bound. A function past the cap is unavailable, never partially proved. */
#define XR_TARGET_INSTRUCTION_MAX_BLOCKS 4096u

/* A jump carries one target in the low half of its immediate and leaves the
 * high half zero. A conditional branch carries the nonzero-condition target in
 * the low half and the zero-condition target in the high half, so both edges
 * are explicit and neither depends on row adjacency. */
#define XR_TARGET_INSTRUCTION_TARGET_IF_NONZERO(immediate)                                         \
    ((uint32_t) ((immediate) & UINT64_C(0xffffffff)))
#define XR_TARGET_INSTRUCTION_TARGET_IF_ZERO(immediate) ((uint32_t) ((immediate) >> 32))
#define XR_TARGET_INSTRUCTION_TARGET_PACK(nonzero, zero)                                           \
    (((uint64_t) (uint32_t) (zero) << 32) | (uint64_t) (uint32_t) (nonzero))

#define XR_TARGET_REQUIRED_FAMILIES                                                                \
    ((uint64_t) (XR_TARGET_FAMILY_SCALAR | XR_TARGET_FAMILY_AGGREGATE |                            \
                 XR_TARGET_FAMILY_CALL_ADAPTER | XR_TARGET_FAMILY_CLOSURE_STORAGE |                \
                 XR_TARGET_FAMILY_COROUTINE_STATE_CALL | XR_TARGET_FAMILY_STRING_LITERAL_STORAGE | \
                 XR_TARGET_FAMILY_DIRECT_LOCAL_CALLEE_STORAGE |                                    \
                 XR_TARGET_FAMILY_CHANNEL_ALLOCATION_STORAGE |                                     \
                 XR_TARGET_FAMILY_CHANNEL_RECEIVE_STORAGE |                                        \
                 XR_TARGET_FAMILY_DIRECT_LOCAL_GO_CALLEE_STORAGE |                                 \
                 XR_TARGET_FAMILY_SOURCE_IMPORT_STORAGE |                                          \
                 XR_TARGET_FAMILY_STRING_BYTE_SLICE_VIEW_STORAGE |                                 \
                 XR_TARGET_FAMILY_DIRECT_LOCAL_UNIT_ENUM_ARGUMENT_STORAGE |                        \
                 XR_TARGET_FAMILY_STRINGBUILDER_APPEND_RUNE_STORAGE |                              \
                 XR_TARGET_FAMILY_STRINGBUILDER_TO_STRING_STORAGE |                                \
                 XR_TARGET_FAMILY_STRINGBUILDER_APPEND_STRING_STORAGE |                            \
                 XR_TARGET_FAMILY_JSON_NAMESPACE_VALUE_STORAGE |                                   \
                 XR_TARGET_FAMILY_DIRECT_LOCAL_STRING_BOUNDARY_STORAGE |                           \
                 XR_TARGET_FAMILY_ARRAY_ALLOCATION_STORAGE |                                       \
                 XR_TARGET_FAMILY_NATIVE_MODULE_NAMESPACE_STORAGE |                                \
                 XR_TARGET_FAMILY_NULLABLE_SCALAR_STORAGE |                                        \
                 XR_TARGET_FAMILY_ARRAY_MEMBER_RESULT_STORAGE |                                    \
                 XR_TARGET_FAMILY_SOURCE_CLASS_OBJECT_STORAGE |                                    \
                 XR_TARGET_FAMILY_SOURCE_CLASS_INSTANCE_STORAGE |                                  \
                 XR_TARGET_FAMILY_SOURCE_CLASS_RECEIVER_STORAGE |                                  \
                 XR_TARGET_FAMILY_SOURCE_CLASS_METHOD_RECEIVER_STORAGE |                           \
                 XR_TARGET_FAMILY_SOURCE_CLASS_ARGUMENT_STORAGE |                                  \
                 XR_TARGET_FAMILY_STRING_CONCAT_RESULT_STORAGE |                                   \
                 XR_TARGET_FAMILY_DIRECT_LOCAL_GO_TASK_RESULT_STORAGE |                            \
                 XR_TARGET_FAMILY_PANIC_CATCH_STORAGE | XR_TARGET_FAMILY_ADT_ENUM_STORAGE |        \
                 XR_TARGET_FAMILY_ARRAY_INTRINSIC_STORAGE |                                        \
                 XR_TARGET_FAMILY_STRING_RUNES_RESULT_STORAGE |                                    \
                 XR_TARGET_FAMILY_DIRECT_LOCAL_ARRAY_REF_ARGUMENT_STORAGE |                        \
                 XR_TARGET_FAMILY_STRING_SLICE_RANGE_RESULT_STORAGE |                              \
                 XR_TARGET_FAMILY_DYNAMIC_ENTRY_EXPECTATION |                                      \
                 XR_TARGET_FAMILY_ARRAY_HOF_RESULT_STORAGE |                                       \
                 XR_TARGET_FAMILY_DIRECT_LOCAL_AGGREGATE_RESULT_STORAGE))

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
    XR_TARGET_CALL_CONVENTION_ARRAY_MEMBER_SCALAR,
    XR_TARGET_CALL_CONVENTION_NATIVE_MODULE_SCALAR,
    XR_TARGET_CALL_CONVENTION_NATIVE_NAMESPACE_YIELDABLE,
    XR_TARGET_CALL_CONVENTION_SOURCE_CLASS_CONSTRUCTOR,
    XR_TARGET_CALL_CONVENTION_ADT_ENUM_CONSTRUCTOR,
    XR_TARGET_CALL_CONVENTION_ARRAY_INTRINSIC,
    XR_TARGET_CALL_CONVENTION_STRING_RUNES,
    XR_TARGET_CALL_CONVENTION_ITERATOR_RUNE_HAS_NEXT,
    XR_TARGET_CALL_CONVENTION_ITERATOR_RUNE_NEXT,
    XR_TARGET_CALL_CONVENTION_RUNE_TO_UINT32,
    XR_TARGET_CALL_CONVENTION_RUNE_IS_WHITESPACE,
    XR_TARGET_CALL_CONVENTION_STRING_SLICE_RANGE,
    XR_TARGET_CALL_CONVENTION_ARRAY_FILL_SCALAR,
    XR_TARGET_CALL_CONVENTION_ARRAY_HOF,
} XrTargetCallConvention;

/* Target dispatch authority. SOURCE_EXPORT names only the public dependency
 * wrapper proven by an ordered SemanticPlan module set; it never identifies a
 * private implementation or reuses a dependency function index as a local
 * index. CHANNEL_CLOSE names a sealed runtime receiver operation.
 * JSON_NAMESPACE_VALUE names the sealed compiler-owned JSON class namespace
 * member, whose receiver is a reserved builtin global rather than a value.
 * ARRAY_MEMBER_SCALAR names a sealed builtin container member on an array
 * receiver whose element carries no reference, so neither a consumed argument
 * nor the element traffic inside the container leaves a reference-count
 * obligation behind the call. A member that hands back its receiver claims no
 * result storage at all: its result is the receiver's own reference, so the row
 * binds no value and every use of that result stays without authority.
 * NATIVE_MODULE_SCALAR names the member the frozen stdlib definition registry
 * binds for one imported native module namespace and selector, whose receiver
 * is a module handle rather than a value and whose arguments and result are all
 * plain scalars, so the row carries no argument intent of its own.
 * SOURCE_CLASS_CONSTRUCTOR names the construction of a declared class through
 * its own class object, taken from the SemanticPlan call target of the same
 * name; it names no callee function and carries no argument, and it never
 * suspends. ADT_ENUM_CONSTRUCTOR names one payload-bearing member of an exact
 * source enum through its frozen enum namespace; its ordered payload recipe is
 * projected by CEmissionPlan rather than reconstructed by CGen. */
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
    XR_TARGET_CALL_TARGET_ARRAY_MEMBER_SCALAR,
    XR_TARGET_CALL_TARGET_NATIVE_MODULE_SCALAR,
    XR_TARGET_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE,
    XR_TARGET_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR,
    XR_TARGET_CALL_TARGET_ADT_ENUM_CONSTRUCTOR,
    XR_TARGET_CALL_TARGET_ARRAY_INTRINSIC,
    XR_TARGET_CALL_TARGET_STRING_RUNES,
    XR_TARGET_CALL_TARGET_ITERATOR_RUNE_HAS_NEXT,
    XR_TARGET_CALL_TARGET_ITERATOR_RUNE_NEXT,
    XR_TARGET_CALL_TARGET_RUNE_TO_UINT32,
    XR_TARGET_CALL_TARGET_RUNE_IS_WHITESPACE,
    XR_TARGET_CALL_TARGET_STRING_SLICE_RANGE,
    XR_TARGET_CALL_TARGET_ARRAY_FILL_SCALAR,
    XR_TARGET_CALL_TARGET_ARRAY_HOF,
} XrTargetCallTargetKind;

typedef enum XrTargetArrayHofKind {
    XR_TARGET_ARRAY_HOF_NONE = 0,
    XR_TARGET_ARRAY_HOF_MAP,
    XR_TARGET_ARRAY_HOF_FILTER,
    XR_TARGET_ARRAY_HOF_REDUCE,
    XR_TARGET_ARRAY_HOF_COUNT,
} XrTargetArrayHofKind;

typedef enum XrTargetArrayIntrinsicKind {
    XR_TARGET_ARRAY_INTRINSIC_NONE = 0,
    XR_TARGET_ARRAY_INTRINSIC_WITH_CAPACITY,
    XR_TARGET_ARRAY_INTRINSIC_FILLED_NEW,
    XR_TARGET_ARRAY_INTRINSIC_COUNT,
} XrTargetArrayIntrinsicKind;

typedef enum XrTargetArrayElementStorage {
    XR_TARGET_ARRAY_STORAGE_NONE = 0,
    XR_TARGET_ARRAY_STORAGE_I8,
    XR_TARGET_ARRAY_STORAGE_U8,
    XR_TARGET_ARRAY_STORAGE_I16,
    XR_TARGET_ARRAY_STORAGE_U16,
    XR_TARGET_ARRAY_STORAGE_I32,
    XR_TARGET_ARRAY_STORAGE_U32,
    XR_TARGET_ARRAY_STORAGE_I64,
    XR_TARGET_ARRAY_STORAGE_U64,
    XR_TARGET_ARRAY_STORAGE_F32,
    XR_TARGET_ARRAY_STORAGE_F64,
    XR_TARGET_ARRAY_STORAGE_BOOL,
    XR_TARGET_ARRAY_STORAGE_RUNE,
    XR_TARGET_ARRAY_STORAGE_COUNT,
} XrTargetArrayElementStorage;

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
    uint8_t array_element_storage;
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
    uint32_t semantic_name;
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
    uint16_t opcode;
    uint8_t operand_count;
    uint8_t reserved;
    uint64_t immediate_bits;
} XrTargetInstructionRecord;

XR_STATIC_ASSERT(XR_TARGET_INSTRUCTION_MAX_STABLE_ID <= UINT16_MAX,
                 "XTP instruction opcode carrier must fit the generated registry");
XR_STATIC_ASSERT(sizeof(XrTargetInstructionRecord) == 32u,
                 "XTP instruction record must remain one compact 32-byte row");

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
    uint16_t callee_register_rep;
    uint16_t callee_memory_rep;
    uint16_t ordinal;
    uint8_t mode;
    uint8_t ownership;
    uint8_t transfer_mode;
    uint8_t flags;
    uint8_t array_element_storage;
    uint8_t reserved8[3];
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
    uint8_t array_intrinsic_kind;
    uint8_t array_element_storage;
    uint8_t array_hof_kind;
    uint8_t array_result_element_storage;
    uint8_t reserved8[3];
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
    /* Function-local first instruction of the frozen resume block, or NONE
     * when this state has no executable Target instruction group. */
    uint32_t resume_instruction;
    uint32_t direct_call;
    uint32_t result_slot;
    uint16_t resume_predecessor_ordinal;
    uint16_t flags;
} XrTargetCoroutineStateRecord;

/* Persistent dynamic-entry authority contains identities and derived ABI
 * facts only. A process-local entry cell or code pointer is never serializable
 * and therefore has no field in this row. */
typedef enum XrTargetEntryValueKind {
    XR_TARGET_ENTRY_VALUE_INVALID = 0,
    XR_TARGET_ENTRY_VALUE_EXACT_I64,
} XrTargetEntryValueKind;

typedef enum XrTargetEntryAdapterKind {
    XR_TARGET_ENTRY_ADAPTER_INVALID = 0,
    XR_TARGET_ENTRY_ADAPTER_IDENTITY,
} XrTargetEntryAdapterKind;

typedef struct XrTargetEntryExpectationRecord {
    XrStableId identity;
    uint32_t id;
    uint32_t call;
    uint32_t abi_schema_version;
    uint16_t parameter_count;
    uint16_t native_abi;
    uint8_t value_kind;
    uint8_t adapter_kind;
    uint16_t flags;
    uint32_t reserved32;
    uint64_t target_data_layout;
    XrFingerprint target_profile_fingerprint;
    XrFingerprint entry_abi_fingerprint;
    XrFingerprint adapter_fingerprint;
} XrTargetEntryExpectationRecord;

/* One immutable observation record is emitted for every executable row.  It
 * copies only identities already proved by the SemanticPlan and TargetPlan;
 * an all-zero optional identity means that the exact source relation does not
 * apply to that row. */
typedef struct XrTargetDebugFactRecord {
    uint32_t id;
    uint32_t instruction;
    uint32_t function;
    uint32_t semantic_operation;
    uint32_t coroutine_state;
    uint32_t source_start_line;
    uint32_t source_start_column;
    uint32_t source_end_line;
    uint32_t source_end_column;
    XrStableId semantic_operation_identity;
    XrStableId source_span_identity;
    XrStableId owner_identity;
    XrStableId coroutine_state_identity;
    XrFingerprint layout_fingerprint;
} XrTargetDebugFactRecord;

XR_STATIC_ASSERT(sizeof(XrTargetEntryExpectationRecord) == 144u,
                 "dynamic entry expectation wire facts must stay compact");

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
XR_FUNC uint64_t xr_target_plan_function_execution_family_mask(const XrTargetPlan *plan,
                                                               uint32_t function);
XR_FUNC const XrTargetInstructionRecord *
xr_target_plan_function_instructions(const XrTargetPlan *plan, uint32_t function, uint32_t *count);

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
XR_TARGET_TABLE_ACCESSOR(entry_expectations, XrTargetEntryExpectationRecord);
XR_TARGET_TABLE_ACCESSOR(debug_facts, XrTargetDebugFactRecord);
#undef XR_TARGET_TABLE_ACCESSOR

#endif  // XR_TARGET_PLAN_H
