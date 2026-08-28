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
#include "../../shared/xr_assertion_plan.h"
#include "../../shared/xr_print_plan.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define XR_SEMANTIC_INDEX_NONE UINT32_MAX
#define XR_SEMANTIC_PROGRAM_PROVENANCE_SCHEMA_VERSION UINT32_C(5)

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
    XR_SEM_CONST_BIGINT,
    XR_SEM_CONST_ENUM_METADATA,
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

typedef enum XrSemanticIntrinsicKind {
    XR_SEM_INTRINSIC_NONE = 0,
    XR_SEM_INTRINSIC_STRING_BYTE_SLICE_VIEW = 1,
    XR_SEM_INTRINSIC_STRINGBUILDER_APPEND_RUNE = 2,
    XR_SEM_INTRINSIC_STRINGBUILDER_TO_STRING = 3,
    XR_SEM_INTRINSIC_STRINGBUILDER_APPEND_STRING = 4,
    XR_SEM_INTRINSIC_JSON_NAMESPACE_VALUE = 5,
    XR_SEM_INTRINSIC_ARRAY_MEMBER_SCALAR = 6,
    XR_SEM_INTRINSIC_NATIVE_MODULE_SCALAR_CALL = 7,
    XR_SEM_INTRINSIC_ARRAY_WITH_CAPACITY = 8,
    XR_SEM_INTRINSIC_ARRAY_FILLED_NEW = 9,
    XR_SEM_INTRINSIC_STRING_RUNES = 10,
    XR_SEM_INTRINSIC_ITERATOR_RUNE_HAS_NEXT = 11,
    XR_SEM_INTRINSIC_ITERATOR_RUNE_NEXT = 12,
    XR_SEM_INTRINSIC_RUNE_TO_UINT32 = 13,
    XR_SEM_INTRINSIC_RUNE_IS_WHITESPACE = 14,
    XR_SEM_INTRINSIC_STRING_SLICE_RANGE = 15,
    XR_SEM_INTRINSIC_ARRAY_FILL_SCALAR = 16,
    XR_SEM_INTRINSIC_ARRAY_HOF = 17,
    XR_SEM_INTRINSIC_PANIC_INFO_CONSTRUCTOR = 18,
    XR_SEM_INTRINSIC_ITERATOR_RUNE_NTH = 19,
    XR_SEM_INTRINSIC_RUNE_TO_STRING = 20,
    XR_SEM_INTRINSIC_MAP_ENTRIES_ITERATOR = 21,
    XR_SEM_INTRINSIC_MAP_ENTRY_ITERATOR_HAS_NEXT = 22,
    XR_SEM_INTRINSIC_MAP_ENTRY_ITERATOR_NEXT = 23,
    XR_SEM_INTRINSIC_ASSERTION = 24,
    XR_SEM_INTRINSIC_NATIVE_TARGET_LEAF_SCALAR_CALL = 25,
    XR_SEM_INTRINSIC_OUTPUT = 26,
    XR_SEM_INTRINSIC_COUNT,
} XrSemanticIntrinsicKind;

/* XI_ASSERTION reserves all eight generic evidence slots as one typed
 * projection of XrAssertionPlan. Other operations retain their established
 * evidence meanings. */
typedef enum XrSemanticAssertionEvidenceSlot {
    XR_SEM_ASSERT_EVIDENCE_SCHEMA = 0,
    XR_SEM_ASSERT_EVIDENCE_BUILTIN_ID = 1,
    XR_SEM_ASSERT_EVIDENCE_KIND = 2,
    XR_SEM_ASSERT_EVIDENCE_FAILURE_CHANNEL = 3,
    XR_SEM_ASSERT_EVIDENCE_FLOW_RULE = 4,
    XR_SEM_ASSERT_EVIDENCE_EQUALITY = 5,
    XR_SEM_ASSERT_EVIDENCE_TARGET = 6,
    XR_SEM_ASSERT_EVIDENCE_CAPABILITIES = 7,
    XR_SEM_ASSERT_EVIDENCE_COUNT = 8,
} XrSemanticAssertionEvidenceSlot;

/* XI_PRINT projects XrPrintPlan into the generic evidence slots the same way.
 * The group's framing is recorded, not each operand's, because the plan owns
 * that decision. */
typedef enum XrSemanticPrintEvidenceSlot {
    XR_SEM_PRINT_EVIDENCE_SCHEMA = 0,
    XR_SEM_PRINT_EVIDENCE_BUILTIN_ID = 1,
    XR_SEM_PRINT_EVIDENCE_SEPARATOR = 2,
    XR_SEM_PRINT_EVIDENCE_TERMINATOR = 3,
    XR_SEM_PRINT_EVIDENCE_TARGET = 4,
    XR_SEM_PRINT_EVIDENCE_CAPABILITIES = 5,
    XR_SEM_PRINT_EVIDENCE_COUNT = 6,
} XrSemanticPrintEvidenceSlot;

typedef enum XrSemanticArrayHofKind {
    XR_SEM_ARRAY_HOF_NONE = 0,
    XR_SEM_ARRAY_HOF_MAP,
    XR_SEM_ARRAY_HOF_FILTER,
    XR_SEM_ARRAY_HOF_REDUCE,
    XR_SEM_ARRAY_HOF_COUNT,
} XrSemanticArrayHofKind;

typedef enum XrSemanticCallTargetKind {
    XR_SEM_CALL_TARGET_DIRECT_LOCAL = 1,
    XR_SEM_CALL_TARGET_NATIVE_YIELDABLE,
    XR_SEM_CALL_TARGET_SOURCE_EXPORT,
    XR_SEM_CALL_TARGET_INDIRECT_CALLABLE,
    XR_SEM_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE,
    XR_SEM_CALL_TARGET_BUILTIN_INSTANCE_YIELDABLE,
    XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_LOCAL,
    XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_OPEN,
    /* Construction of a declared class through its own class object. The row
     * names the declaration it builds and no callee function at all: a class
     * that declares no instance constructor enters no body, and one that does
     * reaches that body through the declaration rather than through this row.
     * A construction whose call may suspend is outside this kind. */
    XR_SEM_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR,
    /* A method call that resolves to one body in this module, on a class that
     * is not declared final. The row states an obligation rather than a
     * conclusion: this call reaches that body PROVIDED no class in the final
     * module graph overrides the method. A module cannot check that -- it sees
     * what it depends on, never what depends on it -- so the layer holding the
     * whole graph decides, and consumes this the way it consumes the local kind
     * once it has. Unconsumed, the row binds nothing. */
    XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_SEALED_CANDIDATE,
    XR_SEM_CALL_TARGET_KIND_COUNT,
} XrSemanticCallTargetKind;

typedef enum XrSemanticSourceClassFlag {
    XR_SEM_SOURCE_CLASS_EXPLICIT_FINAL = 1u << 0,
    XR_SEM_SOURCE_CLASS_RUNTIME_TYPE = 1u << 1,
    XR_SEM_SOURCE_CLASS_GENERIC = 1u << 2,
} XrSemanticSourceClassFlag;

/* Bits of XrSemanticFunctionRecord.flags, named here rather than spelled as
 * numbers at the places that read and write them. */
typedef enum XrSemanticFunctionFlag {
    XR_SEM_FUNCTION_NOTHROW = 1u << 0,
    XR_SEM_FUNCTION_CONTAINS_UNSAFE = 1u << 1,
    XR_SEM_FUNCTION_GENERATOR = 1u << 2,
    XR_SEM_FUNCTION_EXTERN = 1u << 3,
} XrSemanticFunctionFlag;

typedef enum XrSemanticSourceFunctionKind {
    XR_SEM_SOURCE_FUNCTION_NONE = 0,
    XR_SEM_SOURCE_FUNCTION_INSTANCE_METHOD,
    XR_SEM_SOURCE_FUNCTION_STATIC_METHOD,
    XR_SEM_SOURCE_FUNCTION_CONSTRUCTOR,
} XrSemanticSourceFunctionKind;

typedef enum XrSemanticSourceMethodFlag {
    XR_SEM_SOURCE_METHOD_INSTANCE = 1u << 0,
    XR_SEM_SOURCE_METHOD_OPEN_DOMAIN = 1u << 1,
} XrSemanticSourceMethodFlag;

typedef enum XrSemanticImportResolution {
    XR_SEM_IMPORT_RESOLUTION_NONE = 0,
    XR_SEM_IMPORT_RESOLUTION_UNRESOLVED,
    XR_SEM_IMPORT_RESOLUTION_SOURCE_MODULE,
    XR_SEM_IMPORT_RESOLUTION_NATIVE_STDLIB,
    XR_SEM_IMPORT_RESOLUTION_COUNT,
} XrSemanticImportResolution;

typedef enum XrSemanticTypeFlag {
    XR_SEM_TYPE_NULLABLE = 1u << 0,
    XR_SEM_TYPE_CONST = 1u << 1,
    XR_SEM_TYPE_VALUE = 1u << 2,
    XR_SEM_TYPE_LITERAL = 1u << 3,
    XR_SEM_TYPE_REFERENCE_CAPABLE = 1u << 4,
    XR_SEM_TYPE_BORROW_VIEW = 1u << 5,
    XR_SEM_TYPE_OWNERSHIP_ROOT = 1u << 6,
    XR_SEM_TYPE_AGGREGATE_EXACT = 1u << 7,
} XrSemanticTypeFlag;

typedef enum XrSemanticEnumFlag {
    XR_SEM_ENUM_DECLARATION_EXACT = 1u << 0,
    XR_SEM_ENUM_UNIT = 1u << 1,
} XrSemanticEnumFlag;

typedef enum XrSemanticParameterFlag {
    XR_SEM_PARAMETER_REQUIRED = 1u << 0,
    XR_SEM_PARAMETER_VARIADIC = 1u << 1,
    XR_SEM_PARAMETER_RECEIVER_BORROWED = 1u << 2,
    XR_SEM_PARAMETER_READ_PLACE = 1u << 3,
} XrSemanticParameterFlag;

typedef enum XrSemanticCaptureSource {
    XR_SEM_CAPTURE_LOCAL_VALUE = 0,
    XR_SEM_CAPTURE_PARENT_CAPTURE,
} XrSemanticCaptureSource;

typedef enum XrSemanticCaptureKind {
    XR_SEM_CAPTURE_BY_COPY = 0,
    XR_SEM_CAPTURE_BY_IMM_REF,
    XR_SEM_CAPTURE_BY_MUT_CELL,
    XR_SEM_CAPTURE_MODULE_LIVE,
    XR_SEM_CAPTURE_SHARED,
} XrSemanticCaptureKind;

typedef enum XrSemanticCaptureFlag {
    XR_SEM_CAPTURE_NEEDS_CELL = 1u << 0,
    XR_SEM_CAPTURE_MUTABLE = 1u << 1,
    XR_SEM_CAPTURE_REASSIGNED = 1u << 2,
} XrSemanticCaptureFlag;

typedef enum XrSemanticValueCapability {
    XR_SEM_VALUE_MUTABLE = 0,
    XR_SEM_VALUE_CONST,
    XR_SEM_VALUE_SYNC_INTERIOR_MUTABLE,
    XR_SEM_VALUE_CAPABILITY_UNKNOWN,
} XrSemanticValueCapability;

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

typedef enum XrSemanticEntityKind {
    XR_SEM_ENTITY_PACKAGE = 0,
    XR_SEM_ENTITY_MODULE,
    XR_SEM_ENTITY_DECLARATION,
    XR_SEM_ENTITY_TYPE_INSTANTIATION,
    XR_SEM_ENTITY_SHAPE,
    XR_SEM_ENTITY_FIELD,
    XR_SEM_ENTITY_FUNCTION,
    XR_SEM_ENTITY_CLOSURE,
    XR_SEM_ENTITY_NATIVE,
    XR_SEM_ENTITY_OPERATION,
    XR_SEM_ENTITY_ALLOCATION,
    XR_SEM_ENTITY_OWNER,
    XR_SEM_ENTITY_LOAN,
    XR_SEM_ENTITY_DOMAIN,
    XR_SEM_ENTITY_COROUTINE_STATE,
    XR_SEM_ENTITY_DEBUG_SPAN,
    XR_SEM_ENTITY_COROUTINE_LIVE,
    XR_SEM_ENTITY_COROUTINE_ROOT,
    XR_SEM_ENTITY_COROUTINE_DROP,
    XR_SEM_ENTITY_KIND_COUNT,
} XrSemanticEntityKind;

typedef enum XrSemanticEntitySubject {
    XR_SEM_ENTITY_SUBJECT_NONE = 0,
    XR_SEM_ENTITY_SUBJECT_TYPE,
    XR_SEM_ENTITY_SUBJECT_FUNCTION,
    XR_SEM_ENTITY_SUBJECT_PARAMETER,
    XR_SEM_ENTITY_SUBJECT_CAPTURE,
    XR_SEM_ENTITY_SUBJECT_OPERATION,
    XR_SEM_ENTITY_SUBJECT_OWNER,
    XR_SEM_ENTITY_SUBJECT_STORAGE_DOMAIN,
} XrSemanticEntitySubject;

typedef struct XrSemanticEntityRecord {
    XrStableId id;
    const char *canonical_key;
    uint32_t parent;
    uint32_t subject;
    uint32_t ordinal;
    uint16_t kind;
    uint8_t subject_kind;
    uint8_t flags;
} XrSemanticEntityRecord;

typedef struct XrSemanticTypeRecord {
    XrStableId id;
    const char *canonical_key;
    XrStableId source_enum_identity;
    const char *source_enum_key;
    uint32_t kind;
    uint32_t builtin_type;
    uint32_t source_class;
    XrStableId source_class_identity;
    uint32_t child_begin;
    uint32_t aggregate_extent;
    uint32_t aggregate_align;
    uint32_t enum_layout_id;
    uint16_t child_count;
    uint16_t enum_member_count;
    uint8_t scalar_rep;
    uint8_t flags;
    uint8_t enum_flags;
    uint8_t reserved_enum;
} XrSemanticTypeRecord;

typedef struct XrSemanticSourceClassRecord {
    XrStableId id;
    const char *canonical_key;
    XrStableId module;
    const char *module_path;
    const char *name;
    /* The declared parent's name, or NULL when the class declares none. The
     * inheritance edge is a property of the module itself, so it belongs in
     * this frozen snapshot; deciding whether a class has any subclass needs the
     * whole module graph and belongs to the layer that holds one. Names rather
     * than identities: a subclass names the parent it was compiled against, and
     * a graph carrying two classes of one name cannot answer the question at
     * all, so the reader refuses instead of guessing. */
    const char *super_name;
    uint32_t ordinal;
    uint16_t method_count;
    uint8_t flags;
    uint8_t reserved;
} XrSemanticSourceClassRecord;

/* Stable source declaration and its open-world dispatch domain.  The function
 * and class indexes are plan-local; the ID is the cross-XSM authority. */
typedef struct XrSemanticSourceMethodRecord {
    XrStableId id;
    const char *canonical_key;
    const char *name;
    uint32_t source_class;
    uint32_t function;
    uint16_t member_ordinal;
    uint16_t parameter_count;
    uint8_t flags;
    uint8_t reserved[3];
} XrSemanticSourceMethodRecord;

typedef struct XrSemanticFunctionRecord {
    XrStableId id;
    const char *canonical_key;
    const char *name;
    uint32_t return_type;
    uint32_t parent;
    uint32_t parameter_begin;
    uint16_t parameter_count;
    uint16_t child_count;
    uint32_t capture_begin;
    uint16_t capture_count;
    uint16_t reserved;
    uint32_t block_begin;
    uint32_t block_count;
    uint32_t value_begin;
    uint32_t value_count;
    uint32_t semantic_effects;
    uint32_t capability_mask;
    uint32_t source_class;
    uint16_t source_member_ordinal;
    int16_t return_parameter;
    uint8_t return_provenance;
    uint8_t source_kind;
    uint8_t flags;
    /* Whether the runtime enters this function rather than a call in this plan.
     * A module initializer therefore has a boundary no call site can witness,
     * which is why the fact is frozen on the function instead of inferred from
     * the calls that reach it. */
    uint8_t is_module_initializer;
    /* Whether this function's own body carries a coroutine operation.
     *
     * Distinct from being able to suspend and from having coroutine state
     * entities: a function that only *calls* a coroutine has both of those and
     * is still not one itself. This says the body contains the operations, and
     * it is what decides the shape of the function's own C boundary. */
    uint8_t carries_coroutine_ops;
} XrSemanticFunctionRecord;

typedef struct XrSemanticParameterRecord {
    XrStableId id;
    const char *canonical_key;
    uint32_t function;
    uint32_t type;
    uint32_t value;
    uint16_t ordinal;
    uint8_t mode;
    uint8_t ownership;
    uint8_t transfer_mode;
    uint8_t flags;
    uint16_t reserved;
} XrSemanticParameterRecord;

typedef struct XrSemanticCaptureRecord {
    XrStableId id;
    const char *canonical_key;
    const char *name;
    uint32_t function;
    uint32_t source_function;
    uint32_t source_value;
    uint32_t source_capture;
    uint32_t type;
    uint32_t source_type;
    uint32_t source_index;
    uint16_t ordinal;
    uint8_t source;
    uint8_t kind;
    uint8_t storage_domain;
    uint8_t value_capability;
    uint8_t flags;
    uint8_t reserved[1];
} XrSemanticCaptureRecord;

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
    uint8_t import_resolution;
    uint32_t effects;
    uint32_t source_line;
    const char *source_file;
    uint32_t source_start_line;
    uint32_t source_start_column;
    uint32_t source_end_line;
    uint32_t source_end_column;
    uint32_t source_discriminator;
    int64_t semantic_immediate;
    uint32_t constant;
    uint32_t callable_function;
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
    uint32_t view_source_value;
    uint32_t view_element_type;
    int16_t view_source_operand;
    int16_t view_source_parameter;
    uint8_t intrinsic_kind;
    uint8_t view_origin;
    uint8_t view_capability;
    uint8_t view_lifetime;
    uint8_t view_complete;
    uint8_t array_element_storage;
    uint8_t reserved_view[2];
    uint8_t array_hof_kind;
    uint8_t array_result_element_storage;
} XrSemanticOperationRecord;

XR_FUNC bool xr_semantic_operation_assertion_plan(const XrSemanticOperationRecord *operation,
                                                  XrAssertionPlan *out);
XR_FUNC bool xr_semantic_operation_print_plan(const XrSemanticOperationRecord *operation,
                                              XrPrintPlan *out);

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

/*
 * Exact call-site authority. DIRECT_LOCAL is rebuilt from frozen SSA or from a
 * unique lexical SET_SHARED/GET_SHARED slot chain. NATIVE_YIELDABLE is rebuilt
 * from a bare IMPORT_REF and the canonical stdlib binding registry.
 * SOURCE_EXPORT is completed only by the ordered module-set verifier against
 * the dependency's public export table. INDIRECT_CALLABLE freezes only an
 * open function-value dispatch domain and its conservative state obligation;
 * it is not a target identity or execution authority. NATIVE_NAMESPACE_YIELDABLE
 * is rebuilt from a resolver-proven native whole-module import plus the
 * canonical stdlib registry. BUILTIN_INSTANCE_YIELDABLE binds a reserved
 * builtin instance type, selector, and arity without asserting a machine call
 * target. SOURCE_INSTANCE_METHOD_LOCAL is an exact final-class declaration;
 * SOURCE_INSTANCE_METHOD_OPEN is a dependency-verified open dispatch domain,
 * not an execution target. All eight kinds independently authorize coroutine
 * state creation without retaining Xi data.
 */
typedef struct XrSemanticCallTargetRecord {
    XrStableId id;
    const char *canonical_key;
    uint32_t operation;
    uint32_t function;
    uint32_t dependency;
    uint32_t source_export;
    XrStableId export_identity;
    XrStableId callee_function;
    uint32_t callable_type;
    uint8_t kind;
    uint8_t reserved[3];
} XrSemanticCallTargetRecord;

/* Pointer-free provenance for program-semantic rows consumed during Xi to
 * SemanticPlan construction. These records are target-neutral plan facts and
 * participate in the plan fingerprint. */
typedef struct XrSemanticProgramProvenance {
    uint32_t schema;
    uint32_t program_schema;
    uint32_t program_family;
    uint32_t type_count;
    uint32_t type_field_count;
    uint32_t function_count;
    uint32_t call_count;
    uint32_t module_count;
    uint32_t dependency_count;
    uint32_t program_module_row;
    uint32_t program_dependency_binding_count;
    uint32_t reserved;
    XrFingerprint program_fingerprint;
    XrStableId generation_identity;
    XrStableId program_module;
} XrSemanticProgramProvenance;

typedef struct XrSemanticProgramDependencyBinding {
    XrStableId resolver_binding;
    uint32_t program_row;
    uint32_t semantic_dependency;
    uint32_t reserved;
} XrSemanticProgramDependencyBinding;

typedef struct XrSemanticProgramTypeBinding {
    XrStableId program_type;
    XrStableId source_class_identity;
    uint32_t semantic_type;
    uint32_t program_row;
    uint32_t field_begin;
    uint32_t field_count;
    uint8_t kind;
    uint8_t exact_scalar;
    uint8_t flags;
    uint8_t reserved;
} XrSemanticProgramTypeBinding;

typedef struct XrSemanticProgramTypeFieldBinding {
    XrStableId program_owner_type;
    XrStableId program_field_type;
    uint32_t owner_program_row;
    uint32_t field_program_row;
    uint32_t semantic_field_type;
    uint32_t declaration_ordinal;
} XrSemanticProgramTypeFieldBinding;

typedef struct XrSemanticProgramFunctionBinding {
    XrStableId program_function;
    uint32_t semantic_function;
    uint32_t program_row;
    uint8_t flags;
    uint8_t reserved[3];
} XrSemanticProgramFunctionBinding;

typedef struct XrSemanticProgramCallBinding {
    XrStableId program_call;
    XrStableId callsite;
    XrStableId caller_program_function;
    XrStableId callee_program_function;
    XrStableId resolver_binding;
    uint32_t operation;
    uint32_t program_row;
    uint32_t target_function;
    uint32_t program_dependency;
    uint32_t reserved;
} XrSemanticProgramCallBinding;

/* Exact source-module dependency.  The fingerprint is a content address, not
 * a suspendability assertion: consumers must present the matching verified
 * dependency plan to the module-set verifier before this row is executable. */
typedef struct XrSemanticDependencyRecord {
    XrStableId id;
    const char *canonical_key;
    const char *module_path;
    XrStableId module;
    XrFingerprint semantic_fingerprint;
} XrSemanticDependencyRecord;

typedef enum XrSemanticSourceExportKind {
    XR_SEM_SOURCE_EXPORT_FUNCTION = 1,
    XR_SEM_SOURCE_EXPORT_SOURCE_CLASS,
    XR_SEM_SOURCE_EXPORT_KIND_COUNT,
} XrSemanticSourceExportKind;

/* A source export is admitted only when the local root entry uniquely stores
 * the named local closure or declared class object into this shared slot before
 * module activation. KIND selects exactly one local entity index; EXPORTED_ENTITY
 * freezes that entity's stable identity so module-set consumers never infer it
 * from the initializer graph. */
typedef struct XrSemanticSourceExportRecord {
    XrStableId id;
    const char *canonical_key;
    const char *name;
    XrStableId exported_entity;
    uint32_t function;
    uint32_t source_class;
    uint32_t shared_slot;
    uint8_t kind;
    uint8_t reserved[3];
} XrSemanticSourceExportRecord;

XR_FUNC XrSemanticPlan *xr_semantic_plan_retain(XrSemanticPlan *plan);
XR_FUNC void xr_semantic_plan_free(XrSemanticPlan *plan);
XR_FUNC bool xr_semantic_plan_is_frozen(const XrSemanticPlan *plan);
XR_FUNC bool xr_semantic_plan_is_verified(const XrSemanticPlan *plan);
XR_FUNC uint32_t xr_semantic_plan_schema(const XrSemanticPlan *plan);
XR_FUNC XrFingerprint xr_semantic_plan_fingerprint(const XrSemanticPlan *plan);
XR_FUNC XrFingerprint xr_semantic_plan_operation_registry_fingerprint(const XrSemanticPlan *plan);
XR_FUNC XrFingerprint xr_semantic_plan_stdlib_registry_fingerprint(const XrSemanticPlan *plan);
XR_FUNC const XrSemanticProgramProvenance *
xr_semantic_plan_program_provenance(const XrSemanticPlan *plan);
XR_FUNC size_t xr_semantic_plan_program_type_binding_count(const XrSemanticPlan *plan);
XR_FUNC size_t xr_semantic_plan_program_type_field_binding_count(const XrSemanticPlan *plan);
XR_FUNC const XrSemanticProgramTypeBinding *
xr_semantic_plan_program_type_binding(const XrSemanticPlan *plan, uint32_t index);
XR_FUNC const XrSemanticProgramTypeFieldBinding *
xr_semantic_plan_program_type_field_binding(const XrSemanticPlan *plan, uint32_t index);
XR_FUNC const XrSemanticProgramTypeBinding *
xr_semantic_plan_program_type_for_row(const XrSemanticPlan *plan, uint32_t program_row);
XR_FUNC const XrSemanticProgramTypeBinding *
xr_semantic_plan_program_type_for_semantic_type(const XrSemanticPlan *plan, uint32_t semantic_type);
XR_FUNC size_t xr_semantic_plan_program_function_binding_count(const XrSemanticPlan *plan);
XR_FUNC size_t xr_semantic_plan_program_dependency_binding_count(const XrSemanticPlan *plan);
XR_FUNC size_t xr_semantic_plan_program_call_binding_count(const XrSemanticPlan *plan);
XR_FUNC const XrSemanticProgramFunctionBinding *
xr_semantic_plan_program_function_binding(const XrSemanticPlan *plan, uint32_t index);
XR_FUNC const XrSemanticProgramDependencyBinding *
xr_semantic_plan_program_dependency_binding(const XrSemanticPlan *plan, uint32_t index);
XR_FUNC const XrSemanticProgramCallBinding *
xr_semantic_plan_program_call_binding(const XrSemanticPlan *plan, uint32_t index);
XR_FUNC const XrSemanticProgramFunctionBinding *
xr_semantic_plan_program_function_for_semantic_function(const XrSemanticPlan *plan,
                                                        uint32_t semantic_function);
XR_FUNC const XrSemanticProgramCallBinding *
xr_semantic_plan_program_call_for_operation(const XrSemanticPlan *plan, uint32_t operation);
XR_FUNC size_t xr_semantic_plan_type_count(const XrSemanticPlan *plan);
XR_FUNC size_t xr_semantic_plan_source_class_count(const XrSemanticPlan *plan);
XR_FUNC size_t xr_semantic_plan_source_method_count(const XrSemanticPlan *plan);
XR_FUNC size_t xr_semantic_plan_function_count(const XrSemanticPlan *plan);
XR_FUNC size_t xr_semantic_plan_parameter_count(const XrSemanticPlan *plan);
XR_FUNC size_t xr_semantic_plan_capture_count(const XrSemanticPlan *plan);
XR_FUNC size_t xr_semantic_plan_block_count(const XrSemanticPlan *plan);
XR_FUNC size_t xr_semantic_plan_operation_count(const XrSemanticPlan *plan);
XR_FUNC size_t xr_semantic_plan_call_target_count(const XrSemanticPlan *plan);
XR_FUNC size_t xr_semantic_plan_dependency_count(const XrSemanticPlan *plan);
XR_FUNC size_t xr_semantic_plan_source_export_count(const XrSemanticPlan *plan);
XR_FUNC size_t xr_semantic_plan_edge_count(const XrSemanticPlan *plan);
XR_FUNC size_t xr_semantic_plan_constant_count(const XrSemanticPlan *plan);
XR_FUNC size_t xr_semantic_plan_entity_count(const XrSemanticPlan *plan);
XR_FUNC const XrSemanticTypeRecord *xr_semantic_plan_type(const XrSemanticPlan *plan,
                                                          uint32_t index);
XR_FUNC const XrSemanticSourceClassRecord *xr_semantic_plan_source_class(const XrSemanticPlan *plan,
                                                                         uint32_t index);
XR_FUNC const XrSemanticSourceMethodRecord *
xr_semantic_plan_source_method(const XrSemanticPlan *plan, uint32_t index);
XR_FUNC const XrSemanticFunctionRecord *xr_semantic_plan_function(const XrSemanticPlan *plan,
                                                                  uint32_t index);
XR_FUNC const XrSemanticParameterRecord *xr_semantic_plan_parameter(const XrSemanticPlan *plan,
                                                                    uint32_t index);
XR_FUNC const XrSemanticCaptureRecord *xr_semantic_plan_capture(const XrSemanticPlan *plan,
                                                                uint32_t index);
XR_FUNC const XrSemanticBlockRecord *xr_semantic_plan_block(const XrSemanticPlan *plan,
                                                            uint32_t index);
XR_FUNC const XrSemanticOperationRecord *xr_semantic_plan_operation(const XrSemanticPlan *plan,
                                                                    uint32_t index);
XR_FUNC const XrSemanticCallTargetRecord *xr_semantic_plan_call_target(const XrSemanticPlan *plan,
                                                                       uint32_t index);
XR_FUNC const XrSemanticDependencyRecord *xr_semantic_plan_dependency(const XrSemanticPlan *plan,
                                                                      uint32_t index);
XR_FUNC const XrSemanticSourceExportRecord *
xr_semantic_plan_source_export(const XrSemanticPlan *plan, uint32_t index);
XR_FUNC const XrSemanticEdgeRecord *xr_semantic_plan_edge(const XrSemanticPlan *plan,
                                                          uint32_t index);
XR_FUNC const XrSemanticConstantRecord *xr_semantic_plan_constant(const XrSemanticPlan *plan,
                                                                  uint32_t index);
XR_FUNC const XrSemanticEntityRecord *xr_semantic_plan_entity(const XrSemanticPlan *plan,
                                                              uint32_t index);
XR_FUNC const XrSemanticEntityRecord *
xr_semantic_plan_unique_module_entity(const XrSemanticPlan *plan);
XR_FUNC bool xr_semantic_plan_dump_entity(const XrSemanticPlan *plan, XrStableId id, FILE *out);
XR_FUNC const uint32_t *xr_semantic_plan_type_children(const XrSemanticPlan *plan, uint32_t *count);
XR_FUNC const uint32_t *xr_semantic_plan_predecessors(const XrSemanticPlan *plan, uint32_t *count);
XR_FUNC const XrSemanticOperandRecord *xr_semantic_plan_operands(const XrSemanticPlan *plan,
                                                                 uint32_t *count);
XR_FUNC const char *const *xr_semantic_plan_metadata(const XrSemanticPlan *plan, uint32_t *count);
XR_FUNC const XrOwnershipCertificate *xr_semantic_plan_ownership(const XrSemanticPlan *plan);
XR_FUNC bool xr_semantic_plan_dump(const XrSemanticPlan *plan, FILE *out);

#endif  // XR_SEMANTIC_PLAN_H
