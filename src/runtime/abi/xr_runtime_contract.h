/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_runtime_contract.h - Structured canonical runtime ABI contracts
 *
 * KEY CONCEPT:
 *   Runtime compatibility is derived from validated pointer-free schemas.
 *   No caller-authored digest, process address, target string, or C struct
 *   byte image is accepted as an ABI fact.
 */

#ifndef XR_RUNTIME_CONTRACT_H
#define XR_RUNTIME_CONTRACT_H

#include "xr_runtime_descriptor.h"
#include "xr_target_runtime_profile.h"
#include <stddef.h>
#include <stdint.h>

#define XR_RUNTIME_ABI_MAX_OBJECT_KINDS 32
#define XR_RUNTIME_ABI_MAX_OBJECT_FLAGS 32
#define XR_RUNTIME_ABI_MAX_DYNAMIC_TAGS 32
#define XR_RUNTIME_ABI_MAX_RECORD_FIELDS 20
#define XR_RUNTIME_ABI_MAX_ENUM_NAMESPACES 4
#define XR_RUNTIME_ABI_MAX_ENUM_VALUES 32
#define XR_RUNTIME_ABI_MAX_PROVIDER_OPERATIONS 32
#define XR_RUNTIME_ABI_MAX_PROVIDERS 16
#define XR_TARGET_PROVIDER_CALL_ABI_MAX_PARAMETERS 8

typedef enum XrRuntimeEndian {
    XR_RUNTIME_ENDIAN_INVALID = 0,
    XR_RUNTIME_ENDIAN_LITTLE = 1,
    XR_RUNTIME_ENDIAN_BIG = 2,
} XrRuntimeEndian;

typedef enum XrRuntimePaddingPolicy {
    XR_RUNTIME_PADDING_INVALID = 0,
    XR_RUNTIME_PADDING_MUST_BE_ZERO = 1,
} XrRuntimePaddingPolicy;

typedef enum XrRuntimeFieldEncoding {
    XR_RUNTIME_FIELD_ENCODING_INVALID = 0,
    XR_RUNTIME_FIELD_UNSIGNED = 1,
    XR_RUNTIME_FIELD_SIGNED_TWOS_COMPLEMENT = 2,
    XR_RUNTIME_FIELD_BITSET = 3,
    XR_RUNTIME_FIELD_STABLE_ID = 4,
    XR_RUNTIME_FIELD_FINGERPRINT = 5,
    XR_RUNTIME_FIELD_OPAQUE_BITS = 6,
} XrRuntimeFieldEncoding;

typedef enum XrRuntimeFieldAtomicity {
    XR_RUNTIME_FIELD_ATOMICITY_INVALID = 0,
    XR_RUNTIME_FIELD_PLAIN = 1,
    XR_RUNTIME_FIELD_DOMAIN_CONDITIONAL = 2,
} XrRuntimeFieldAtomicity;

typedef enum XrRuntimeIndexSemantics {
    XR_RUNTIME_INDEX_NONE = 0,
    XR_RUNTIME_INDEX_VERIFIED_TABLE = 1,
} XrRuntimeIndexSemantics;

typedef enum XrRuntimeAbiFieldRole {
    XR_RUNTIME_FIELD_ROLE_INVALID = 0,
    XR_RUNTIME_FIELD_HEADER_RC,
    XR_RUNTIME_FIELD_HEADER_OBJECT_KIND,
    XR_RUNTIME_FIELD_HEADER_FLAGS,
    XR_RUNTIME_FIELD_HEADER_LAYOUT_ID,
    XR_RUNTIME_FIELD_HEADER_DOMAIN_ID,
    XR_RUNTIME_FIELD_DYN_TAG,
    XR_RUNTIME_FIELD_DYN_FLAGS,
    XR_RUNTIME_FIELD_DYN_PAYLOAD,
    XR_RUNTIME_FIELD_DOMAIN_CONTRACT_ID,
    XR_RUNTIME_FIELD_DOMAIN_INSTANCE_ID,
    XR_RUNTIME_FIELD_DOMAIN_SEMANTIC,
    XR_RUNTIME_FIELD_DOMAIN_MATERIALIZATION,
    XR_RUNTIME_FIELD_EXTENT_SCHEMA,
    XR_RUNTIME_FIELD_EXTENT_ID,
    XR_RUNTIME_FIELD_EXTENT_LAYOUT_ID,
    XR_RUNTIME_FIELD_EXTENT_GROUP_ID,
    XR_RUNTIME_FIELD_EXTENT_PROVIDER_ID,
    XR_RUNTIME_FIELD_EXTENT_TAIL_OFFSET,
    XR_RUNTIME_FIELD_EXTENT_STRIDE,
    XR_RUNTIME_FIELD_EXTENT_OPERAND_INDEX,
    XR_RUNTIME_FIELD_EXTENT_PART_INDEX,
    XR_RUNTIME_FIELD_EXTENT_PART_COUNT,
    XR_RUNTIME_FIELD_EXTENT_KIND,
    XR_RUNTIME_FIELD_EXTENT_FINGERPRINT,
    XR_RUNTIME_FIELD_LAYOUT_SCHEMA,
    XR_RUNTIME_FIELD_LAYOUT_DESCRIPTOR_ID,
    XR_RUNTIME_FIELD_LAYOUT_ID,
    XR_RUNTIME_FIELD_LAYOUT_OBJECT_KIND_ID,
    XR_RUNTIME_FIELD_LAYOUT_EXTENT_ID,
    XR_RUNTIME_FIELD_LAYOUT_ROOT_PLAN_ID,
    XR_RUNTIME_FIELD_LAYOUT_DESTRUCTOR_ID,
    XR_RUNTIME_FIELD_LAYOUT_CLONE_ID,
    XR_RUNTIME_FIELD_LAYOUT_EQ_HASH_ID,
    XR_RUNTIME_FIELD_LAYOUT_EXTENT_FINGERPRINT,
    XR_RUNTIME_FIELD_LAYOUT_FIXED_PREFIX_SIZE,
    XR_RUNTIME_FIELD_LAYOUT_ALIGNMENT,
    XR_RUNTIME_FIELD_LAYOUT_SEMANTIC_DOMAINS,
    XR_RUNTIME_FIELD_LAYOUT_MATERIALIZATIONS,
    XR_RUNTIME_FIELD_LAYOUT_FLAGS,
    XR_RUNTIME_FIELD_LAYOUT_FINGERPRINT,
    XR_RUNTIME_FIELD_LIMIT_MAX_ALLOCATION_BYTES,
    XR_RUNTIME_FIELD_LIMIT_MAX_ALIGNMENT,
    XR_RUNTIME_FIELD_EVALUATED_EXTENT_ID,
    XR_RUNTIME_FIELD_EVALUATED_EXTENT_FINGERPRINT,
    XR_RUNTIME_FIELD_EVALUATED_BYTES,
    XR_RUNTIME_FIELD_EVALUATED_OPERAND,
    XR_RUNTIME_FIELD_EVALUATED_ALIGNMENT,
    XR_RUNTIME_FIELD_EVALUATED_PART_INDEX,
    XR_RUNTIME_FIELD_EVALUATED_PART_COUNT,
    XR_RUNTIME_FIELD_GROUP_ID,
    XR_RUNTIME_FIELD_GROUP_PART_COUNT,
    XR_RUNTIME_FIELD_GROUP_FINGERPRINT,
    XR_RUNTIME_FIELD_ROLE_COUNT,
} XrRuntimeAbiFieldRole;

typedef struct XrRuntimePhysicalFieldAbi {
    uint16_t role;
    uint16_t offset;
    uint16_t width;
    uint16_t alignment;
    uint8_t encoding;
    uint8_t atomicity;
    uint8_t index_semantics;
    uint8_t reserved8;
    uint32_t reserved32;
} XrRuntimePhysicalFieldAbi;

typedef enum XrRuntimeRcPolarity {
    XR_RUNTIME_RC_POLARITY_INVALID = 0,
    XR_RUNTIME_RC_OWNED_POSITIVE = 1,
} XrRuntimeRcPolarity;

typedef enum XrRuntimeRcStickyComparison {
    XR_RUNTIME_RC_STICKY_INVALID = 0,
    XR_RUNTIME_RC_STICKY_LESS_OR_EQUAL = 1,
} XrRuntimeRcStickyComparison;

typedef enum XrRuntimeRcAccessMode {
    XR_RUNTIME_RC_ACCESS_INVALID = 0,
    XR_RUNTIME_RC_ACCESS_PLAIN = 1,
    XR_RUNTIME_RC_ACCESS_ATOMIC = 2,
} XrRuntimeRcAccessMode;

typedef enum XrRuntimeMemoryOrder {
    XR_RUNTIME_MEMORY_ORDER_INVALID = 0,
    XR_RUNTIME_MEMORY_ORDER_RELAXED = 1,
    XR_RUNTIME_MEMORY_ORDER_ACQUIRE = 2,
    XR_RUNTIME_MEMORY_ORDER_RELEASE = 3,
} XrRuntimeMemoryOrder;

typedef struct XrRuntimeRcAbi {
    int32_t initial_value;
    int32_t retain_delta;
    int32_t release_delta;
    int32_t sticky_sentinel;
    int32_t sticky_band_boundary;
    uint8_t polarity;
    uint8_t sticky_comparison;
    uint8_t local_access;
    uint8_t shared_access;
    uint8_t shared_retain_order;
    uint8_t shared_release_order;
    uint8_t shared_destroy_order;
    uint8_t reserved8;
    uint32_t reserved32;
} XrRuntimeRcAbi;

typedef struct XrRuntimeObjectKindAbiEntry {
    XrStableId stable_id;
    uint64_t encoding;
    uint64_t reserved;
} XrRuntimeObjectKindAbiEntry;

typedef struct XrRuntimeObjectKindAbi {
    uint64_t invalid_encoding;
    uint16_t entry_count;
    uint8_t encoding_width;
    uint8_t reserved8;
    uint32_t reserved32;
    XrRuntimeObjectKindAbiEntry entries[XR_RUNTIME_ABI_MAX_OBJECT_KINDS];
} XrRuntimeObjectKindAbi;

typedef struct XrRuntimeObjectFlagAbiEntry {
    XrStableId stable_id;
    uint64_t bit;
    uint16_t exclusivity_group;
    uint16_t reserved16;
    uint32_t reserved32;
} XrRuntimeObjectFlagAbiEntry;

typedef struct XrRuntimeObjectFlagAbi {
    uint64_t valid_mask;
    uint64_t reserved_zero_mask;
    uint16_t entry_count;
    uint8_t encoding_width;
    uint8_t reserved8;
    uint32_t reserved32;
    XrRuntimeObjectFlagAbiEntry entries[XR_RUNTIME_ABI_MAX_OBJECT_FLAGS];
} XrRuntimeObjectFlagAbi;

typedef struct XrRuntimeTableIndexAbi {
    uint64_t invalid_encoding;
    uint8_t encoding_width;
    uint8_t semantics;
    uint16_t reserved16;
    uint32_t reserved32;
} XrRuntimeTableIndexAbi;

#define XR_RUNTIME_OBJECT_HEADER_FIELD_COUNT 5

typedef struct XrRuntimeObjectHeaderAbi {
    uint32_t schema_version;
    uint16_t size;
    uint16_t alignment;
    uint8_t target_endian;
    uint8_t padding_policy;
    uint16_t reserved16;
    uint32_t reserved32;
    XrRuntimePhysicalFieldAbi fields[XR_RUNTIME_OBJECT_HEADER_FIELD_COUNT];
    XrRuntimeRcAbi rc;
    XrRuntimeObjectKindAbi object_kinds;
    XrRuntimeObjectFlagAbi flags;
    XrRuntimeTableIndexAbi layout_id;
    XrRuntimeTableIndexAbi domain_id;
    uint64_t reserved[2];
} XrRuntimeObjectHeaderAbi;

typedef enum XrRuntimeDynamicPayloadKind {
    XR_RUNTIME_DYN_PAYLOAD_INVALID = 0,
    XR_RUNTIME_DYN_PAYLOAD_NONE = 1,
    XR_RUNTIME_DYN_PAYLOAD_INLINE_BITS = 2,
    XR_RUNTIME_DYN_PAYLOAD_OBJECT_REFERENCE = 3,
} XrRuntimeDynamicPayloadKind;

typedef struct XrRuntimeDynamicTagAbiEntry {
    XrStableId stable_id;
    uint64_t encoding;
    uint64_t required_flags;
    uint64_t allowed_flags;
    uint8_t payload_kind;
    uint8_t reserved8[7];
} XrRuntimeDynamicTagAbiEntry;

#define XR_RUNTIME_DYNAMIC_FIELD_COUNT 3

typedef struct XrRuntimeDynamicValueAbi {
    uint32_t schema_version;
    uint16_t size;
    uint16_t alignment;
    uint8_t target_endian;
    uint8_t padding_policy;
    uint8_t tag_encoding_width;
    uint8_t flags_encoding_width;
    uint8_t object_reference_width;
    uint8_t reserved8[3];
    uint64_t invalid_tag;
    uint64_t null_tag;
    uint64_t object_reference_tag;
    uint64_t valid_flags_mask;
    uint64_t reserved_zero_mask;
    uint16_t tag_count;
    uint16_t reserved16;
    uint32_t reserved32;
    XrRuntimePhysicalFieldAbi fields[XR_RUNTIME_DYNAMIC_FIELD_COUNT];
    XrRuntimeDynamicTagAbiEntry tags[XR_RUNTIME_ABI_MAX_DYNAMIC_TAGS];
    uint64_t reserved[2];
} XrRuntimeDynamicValueAbi;

typedef struct XrRuntimeEnumValueAbi {
    XrStableId stable_id;
    uint64_t encoding;
    uint64_t reserved;
} XrRuntimeEnumValueAbi;

typedef enum XrRuntimeEnumNamespaceKind {
    XR_RUNTIME_NAMESPACE_INVALID = 0,
    XR_RUNTIME_NAMESPACE_ENUM = 1,
    XR_RUNTIME_NAMESPACE_BITMASK = 2,
    XR_RUNTIME_NAMESPACE_SENTINEL = 3,
} XrRuntimeEnumNamespaceKind;

typedef enum XrRuntimeEnumNamespaceRole {
    XR_RUNTIME_NAMESPACE_ROLE_INVALID = 0,
    XR_RUNTIME_NAMESPACE_SEMANTIC_DOMAIN = 1,
    XR_RUNTIME_NAMESPACE_MATERIALIZATION = 2,
    XR_RUNTIME_NAMESPACE_EXTENT_KIND = 3,
    XR_RUNTIME_NAMESPACE_EXTENT_OPERAND = 4,
    XR_RUNTIME_NAMESPACE_LAYOUT_FLAGS = 5,
    XR_RUNTIME_NAMESPACE_STATUS = 6,
} XrRuntimeEnumNamespaceRole;

typedef struct XrRuntimeEnumNamespaceAbi {
    uint64_t invalid_encoding;
    uint64_t valid_mask;
    uint64_t reserved_zero_mask;
    uint16_t role;
    uint16_t entry_count;
    uint8_t kind;
    uint8_t encoding_width;
    uint16_t reserved16;
    uint32_t reserved32;
    XrRuntimeEnumValueAbi entries[XR_RUNTIME_ABI_MAX_ENUM_VALUES];
} XrRuntimeEnumNamespaceAbi;

typedef enum XrRuntimeRecordKind {
    XR_RUNTIME_RECORD_INVALID = 0,
    XR_RUNTIME_RECORD_DOMAIN_IDENTITY = 1,
    XR_RUNTIME_RECORD_EXTENT_DESCRIPTOR = 2,
    XR_RUNTIME_RECORD_LAYOUT_DESCRIPTOR = 3,
    XR_RUNTIME_RECORD_EXTENT_LIMITS = 4,
    XR_RUNTIME_RECORD_EVALUATED_EXTENT = 5,
    XR_RUNTIME_RECORD_EXTENT_GROUP_SUMMARY = 6,
} XrRuntimeRecordKind;

typedef struct XrRuntimeRecordAbi {
    uint32_t schema_version;
    uint16_t record_kind;
    uint16_t size;
    uint16_t alignment;
    uint16_t field_count;
    uint16_t namespace_count;
    uint16_t reserved16;
    uint32_t reserved32;
    XrRuntimePhysicalFieldAbi fields[XR_RUNTIME_ABI_MAX_RECORD_FIELDS];
    XrRuntimeEnumNamespaceAbi namespaces[XR_RUNTIME_ABI_MAX_ENUM_NAMESPACES];
    uint64_t reserved[2];
} XrRuntimeRecordAbi;

typedef enum XrRuntimeProviderErrorNormalization {
    XR_RUNTIME_PROVIDER_ERROR_INVALID = 0,
    XR_RUNTIME_PROVIDER_ERROR_NON_OK_TO_REJECTED = 1,
} XrRuntimeProviderErrorNormalization;

typedef struct XrRuntimeExtentProviderCallbackAbi {
    uint32_t schema_version;
    XrStableId contract_id;
    uint16_t provider_id_width;
    uint16_t operand_element_width;
    uint16_t operand_count_width;
    uint16_t result_width;
    uint8_t error_normalization;
    uint8_t reserved8;
    uint16_t reserved16;
    uint32_t reserved32;
    XrRuntimeEnumNamespaceAbi status_namespace;
    uint64_t reserved[2];
} XrRuntimeExtentProviderCallbackAbi;

typedef enum XrRuntimeCheckedArithmeticPolicy {
    XR_RUNTIME_CHECKED_ARITHMETIC_INVALID = 0,
    XR_RUNTIME_CHECKED_ARITHMETIC_REJECT_OVERFLOW = 1,
} XrRuntimeCheckedArithmeticPolicy;

typedef enum XrRuntimeAlignmentPolicy {
    XR_RUNTIME_ALIGNMENT_POLICY_INVALID = 0,
    XR_RUNTIME_ALIGNMENT_POWER_OF_TWO_REJECT_OVERFLOW = 1,
} XrRuntimeAlignmentPolicy;

typedef enum XrRuntimeUnknownEnumPolicy {
    XR_RUNTIME_UNKNOWN_ENUM_INVALID = 0,
    XR_RUNTIME_UNKNOWN_ENUM_REJECT = 1,
} XrRuntimeUnknownEnumPolicy;

typedef enum XrRuntimeReservedZeroPolicy {
    XR_RUNTIME_RESERVED_ZERO_INVALID = 0,
    XR_RUNTIME_RESERVED_ZERO_REJECT = 1,
} XrRuntimeReservedZeroPolicy;

typedef struct XrRuntimeAbiContract {
    uint32_t schema_version;
    uint16_t stable_id_width;
    uint16_t fingerprint_width;
    uint16_t pointer_width;
    uint8_t canonical_serialization_endian;
    uint8_t target_endian;
    uint8_t checked_arithmetic_policy;
    uint8_t alignment_policy;
    uint8_t unknown_enum_policy;
    uint8_t reserved_zero_policy;
    uint16_t reserved16;
    uint32_t reserved32;
    XrRuntimeObjectHeaderAbi object_header;
    XrRuntimeDynamicValueAbi dynamic_value;
    XrRuntimeRecordAbi domain_identity;
    XrRuntimeRecordAbi extent_descriptor;
    XrRuntimeRecordAbi layout_descriptor;
    XrRuntimeRecordAbi extent_limits;
    XrRuntimeRecordAbi evaluated_extent;
    XrRuntimeRecordAbi extent_group_summary;
    XrRuntimeExtentProviderCallbackAbi extent_provider_callback;
    uint64_t reserved[2];
} XrRuntimeAbiContract;

typedef enum XrTargetProviderFlags {
    XR_TARGET_PROVIDER_AVAILABLE_HOSTED = UINT32_C(1) << 0,
    XR_TARGET_PROVIDER_AVAILABLE_FREESTANDING = UINT32_C(1) << 1,
} XrTargetProviderFlags;

#define XR_TARGET_PROVIDER_FLAGS_ALL                                                    \
    (XR_TARGET_PROVIDER_AVAILABLE_HOSTED | XR_TARGET_PROVIDER_AVAILABLE_FREESTANDING)

typedef enum XrTargetProviderEffectFlags {
    XR_TARGET_PROVIDER_EFFECT_ALLOCATES = UINT32_C(1) << 0,
    XR_TARGET_PROVIDER_EFFECT_DEALLOCATES = UINT32_C(1) << 1,
    XR_TARGET_PROVIDER_EFFECT_BLOCKS = UINT32_C(1) << 2,
    XR_TARGET_PROVIDER_EFFECT_IO = UINT32_C(1) << 3,
    XR_TARGET_PROVIDER_EFFECT_SCHEDULES = UINT32_C(1) << 4,
    XR_TARGET_PROVIDER_EFFECT_PANICS = UINT32_C(1) << 5,
} XrTargetProviderEffectFlags;

#define XR_TARGET_PROVIDER_EFFECT_FLAGS_ALL                                             \
    (XR_TARGET_PROVIDER_EFFECT_ALLOCATES | XR_TARGET_PROVIDER_EFFECT_DEALLOCATES |       \
     XR_TARGET_PROVIDER_EFFECT_BLOCKS | XR_TARGET_PROVIDER_EFFECT_IO |                   \
     XR_TARGET_PROVIDER_EFFECT_SCHEDULES | XR_TARGET_PROVIDER_EFFECT_PANICS)

typedef enum XrTargetProviderLifetimeFlags {
    XR_TARGET_PROVIDER_LIFETIME_RETURNS_OWNED = UINT32_C(1) << 0,
    XR_TARGET_PROVIDER_LIFETIME_CONSUMES_OWNED = UINT32_C(1) << 1,
    XR_TARGET_PROVIDER_LIFETIME_BORROWS = UINT32_C(1) << 2,
    XR_TARGET_PROVIDER_LIFETIME_CALLBACK = UINT32_C(1) << 3,
} XrTargetProviderLifetimeFlags;

#define XR_TARGET_PROVIDER_LIFETIME_FLAGS_ALL                                           \
    (XR_TARGET_PROVIDER_LIFETIME_RETURNS_OWNED |                                        \
     XR_TARGET_PROVIDER_LIFETIME_CONSUMES_OWNED | XR_TARGET_PROVIDER_LIFETIME_BORROWS | \
     XR_TARGET_PROVIDER_LIFETIME_CALLBACK)

typedef enum XrTargetProviderFailureFlags {
    XR_TARGET_PROVIDER_FAILURE_RETURNS_STATUS = UINT32_C(1) << 0,
    XR_TARGET_PROVIDER_FAILURE_PANICS = UINT32_C(1) << 1,
    XR_TARGET_PROVIDER_FAILURE_NO_RETURN = UINT32_C(1) << 2,
} XrTargetProviderFailureFlags;

#define XR_TARGET_PROVIDER_FAILURE_FLAGS_ALL                                            \
    (XR_TARGET_PROVIDER_FAILURE_RETURNS_STATUS | XR_TARGET_PROVIDER_FAILURE_PANICS |     \
     XR_TARGET_PROVIDER_FAILURE_NO_RETURN)

typedef enum XrTargetProviderCallingConvention {
    XR_TARGET_PROVIDER_CALLING_CONVENTION_INVALID = 0,
    XR_TARGET_PROVIDER_CALLING_CONVENTION_C = 1,
} XrTargetProviderCallingConvention;

typedef enum XrTargetProviderCallValueKind {
    XR_TARGET_PROVIDER_CALL_VALUE_INVALID = 0,
    XR_TARGET_PROVIDER_CALL_VALUE_VOID = 1,
    XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER = 2,
    XR_TARGET_PROVIDER_CALL_VALUE_UNSIGNED_INTEGER = 3,
    XR_TARGET_PROVIDER_CALL_VALUE_IEEE_FLOAT = 4,
    XR_TARGET_PROVIDER_CALL_VALUE_DATA_ADDRESS = 5,
    XR_TARGET_PROVIDER_CALL_VALUE_CODE_ADDRESS = 6,
} XrTargetProviderCallValueKind;

typedef enum XrTargetProviderCallOwnership {
    XR_TARGET_PROVIDER_CALL_OWNERSHIP_INVALID = 0,
    XR_TARGET_PROVIDER_CALL_OWNERSHIP_NONE = 1,
    XR_TARGET_PROVIDER_CALL_OWNERSHIP_BORROWED = 2,
    XR_TARGET_PROVIDER_CALL_OWNERSHIP_CONSUMED = 3,
    XR_TARGET_PROVIDER_CALL_OWNERSHIP_RETURNED_OWNED = 4,
} XrTargetProviderCallOwnership;

typedef enum XrTargetProviderCallSlotFlags {
    XR_TARGET_PROVIDER_CALL_SLOT_NULLABLE = UINT8_C(1) << 0,
    XR_TARGET_PROVIDER_CALL_SLOT_CONST_POINTEE = UINT8_C(1) << 1,
} XrTargetProviderCallSlotFlags;

#define XR_TARGET_PROVIDER_CALL_SLOT_FLAGS_ALL                                          \
    (XR_TARGET_PROVIDER_CALL_SLOT_NULLABLE | XR_TARGET_PROVIDER_CALL_SLOT_CONST_POINTEE)

typedef struct XrTargetProviderCallSlotAbi {
    uint8_t value_kind;
    uint8_t width;
    uint8_t alignment;
    uint8_t ownership;
    uint8_t flags;
    uint8_t reserved8[3];
    uint64_t reserved64;
} XrTargetProviderCallSlotAbi;

typedef struct XrTargetProviderCallAbiContract {
    uint32_t schema_version;
    uint16_t parameter_count;
    uint8_t calling_convention;
    uint8_t target_endian;
    uint8_t pointer_width;
    uint8_t pointer_alignment;
    uint8_t variadic;
    uint8_t reserved8;
    uint32_t reserved32;
    XrTargetProviderCallSlotAbi result;
    XrTargetProviderCallSlotAbi parameters[XR_TARGET_PROVIDER_CALL_ABI_MAX_PARAMETERS];
    uint64_t reserved[2];
} XrTargetProviderCallAbiContract;

typedef struct XrTargetProviderOperationContract {
    XrStableId stable_id;
    XrTargetProviderCallAbiContract call_abi;
    uint32_t effect_flags;
    uint32_t lifetime_flags;
    uint32_t failure_flags;
    uint32_t reserved32;
    uint64_t reserved64;
} XrTargetProviderOperationContract;

typedef enum XrTargetProviderPanicBehavior {
    XR_TARGET_PROVIDER_PANIC_INVALID = 0,
    XR_TARGET_PROVIDER_PANIC_NO_RETURN = 1,
    XR_TARGET_PROVIDER_PANIC_UNWINDS = 2,
} XrTargetProviderPanicBehavior;

typedef struct XrTargetProviderContract {
    uint32_t schema_version;
    XrStableId contract_id;
    uint32_t abi_schema_version;
    uint32_t flags;
    uint16_t operation_count;
    uint8_t runtime_profile;
    uint8_t provider_kind;
    uint32_t allocator_max_alignment;
    uint8_t allocator_sized_free;
    uint8_t allocator_zeroed_allocation;
    uint8_t allocator_thread_safe;
    uint8_t panic_behavior;
    uint32_t reserved32;
    XrTargetProviderOperationContract operations[XR_RUNTIME_ABI_MAX_PROVIDER_OPERATIONS];
    uint64_t reserved[2];
} XrTargetProviderContract;

XR_FUNC XrRuntimeAbiStatus xr_runtime_object_header_abi_fingerprint(
    const XrRuntimeObjectHeaderAbi *abi, XrFingerprint *out);
XR_FUNC XrRuntimeAbiStatus xr_runtime_abi_contract_fingerprint(
    const XrRuntimeAbiContract *abi, XrFingerprint *out);
XR_FUNC XrRuntimeAbiStatus xr_target_provider_call_abi_fingerprint(
    const XrTargetProviderCallAbiContract *abi, XrFingerprint *out);
XR_FUNC XrRuntimeAbiStatus xr_target_provider_set_fingerprint(
    const XrTargetProviderContract *providers, size_t provider_count,
    uint64_t *out_provider_mask, XrFingerprint *out);

#endif  // XR_RUNTIME_CONTRACT_H
