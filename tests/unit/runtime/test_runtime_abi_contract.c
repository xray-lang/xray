/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_runtime_abi_contract.c - Structured runtime ABI fingerprint tests
 */

#include "runtime/abi/xr_runtime_contract.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, message)                                                         \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "FAIL: %s\n", message);                                      \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

static XrStableId make_id(uint8_t seed) {
    XrStableId id = {{0}};
    id.bytes[0] = seed;
    for (size_t i = 1; i < sizeof(id.bytes); i++)
        id.bytes[i] = (uint8_t) (seed + i);
    return id;
}

static XrFingerprint make_fingerprint(uint8_t seed) {
    XrFingerprint fingerprint = {{0}};
    for (size_t i = 0; i < sizeof(fingerprint.bytes); i++)
        fingerprint.bytes[i] = (uint8_t) (seed + i);
    return fingerprint;
}

static bool fingerprint_equal(XrFingerprint left, XrFingerprint right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static void print_fingerprint(const char *label, XrFingerprint fingerprint) {
    fprintf(stderr, "%s =", label);
    for (size_t i = 0; i < sizeof(fingerprint.bytes); i++)
        fprintf(stderr, " %02x", fingerprint.bytes[i]);
    fputc('\n', stderr);
}

static XrRuntimePhysicalFieldAbi make_field(uint16_t role, uint16_t offset,
                                            uint16_t width, uint16_t alignment,
                                            uint8_t encoding) {
    XrRuntimePhysicalFieldAbi field = {
        .role = role,
        .offset = offset,
        .width = width,
        .alignment = alignment,
        .encoding = encoding,
        .atomicity = XR_RUNTIME_FIELD_PLAIN,
        .index_semantics = XR_RUNTIME_INDEX_NONE,
    };
    return field;
}

static XrRuntimeObjectHeaderAbi make_object_header_abi(void) {
    XrRuntimeObjectHeaderAbi abi = {
        .schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .size = 16,
        .alignment = 4,
        .target_endian = XR_RUNTIME_ENDIAN_LITTLE,
        .padding_policy = XR_RUNTIME_PADDING_MUST_BE_ZERO,
    };
    abi.fields[0] = make_field(XR_RUNTIME_FIELD_HEADER_RC, 0, 4, 4,
                               XR_RUNTIME_FIELD_SIGNED_TWOS_COMPLEMENT);
    abi.fields[0].atomicity = XR_RUNTIME_FIELD_DOMAIN_CONDITIONAL;
    abi.fields[1] = make_field(XR_RUNTIME_FIELD_HEADER_OBJECT_KIND, 4, 2, 2,
                               XR_RUNTIME_FIELD_UNSIGNED);
    abi.fields[2] = make_field(XR_RUNTIME_FIELD_HEADER_FLAGS, 6, 2, 2,
                               XR_RUNTIME_FIELD_BITSET);
    abi.fields[3] = make_field(XR_RUNTIME_FIELD_HEADER_LAYOUT_ID, 8, 4, 4,
                               XR_RUNTIME_FIELD_UNSIGNED);
    abi.fields[3].index_semantics = XR_RUNTIME_INDEX_VERIFIED_TABLE;
    abi.fields[4] = make_field(XR_RUNTIME_FIELD_HEADER_DOMAIN_ID, 12, 4, 4,
                               XR_RUNTIME_FIELD_UNSIGNED);
    abi.fields[4].index_semantics = XR_RUNTIME_INDEX_VERIFIED_TABLE;

    abi.rc = (XrRuntimeRcAbi) {
        .initial_value = 1,
        .retain_delta = 1,
        .release_delta = -1,
        .sticky_sentinel = -1,
        .sticky_band_boundary = -1,
        .polarity = XR_RUNTIME_RC_OWNED_POSITIVE,
        .sticky_comparison = XR_RUNTIME_RC_STICKY_LESS_OR_EQUAL,
        .local_access = XR_RUNTIME_RC_ACCESS_PLAIN,
        .shared_access = XR_RUNTIME_RC_ACCESS_ATOMIC,
        .shared_retain_order = XR_RUNTIME_MEMORY_ORDER_RELAXED,
        .shared_release_order = XR_RUNTIME_MEMORY_ORDER_RELEASE,
        .shared_destroy_order = XR_RUNTIME_MEMORY_ORDER_ACQUIRE,
    };
    abi.object_kinds.invalid_encoding = 0;
    abi.object_kinds.encoding_width = 2;
    abi.object_kinds.entry_count = 3;
    for (size_t i = 0; i < abi.object_kinds.entry_count; i++) {
        abi.object_kinds.entries[i].stable_id = make_id((uint8_t) (1 + i));
        abi.object_kinds.entries[i].encoding = 1 + i;
    }
    abi.flags.encoding_width = 2;
    abi.flags.valid_mask = 7;
    abi.flags.reserved_zero_mask = UINT16_MAX & ~UINT64_C(7);
    abi.flags.entry_count = 3;
    for (size_t i = 0; i < abi.flags.entry_count; i++) {
        abi.flags.entries[i].stable_id = make_id((uint8_t) (10 + i));
        abi.flags.entries[i].bit = UINT64_C(1) << i;
    }
    abi.flags.entries[0].exclusivity_group = 1;
    abi.flags.entries[1].exclusivity_group = 1;
    abi.layout_id = (XrRuntimeTableIndexAbi) {
        .invalid_encoding = 0,
        .encoding_width = 4,
        .semantics = XR_RUNTIME_INDEX_VERIFIED_TABLE,
    };
    abi.domain_id = abi.layout_id;
    return abi;
}

static XrRuntimeDynamicValueAbi make_dynamic_value_abi(void) {
    XrRuntimeDynamicValueAbi abi = {
        .schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .size = 16,
        .alignment = 8,
        .target_endian = XR_RUNTIME_ENDIAN_LITTLE,
        .padding_policy = XR_RUNTIME_PADDING_MUST_BE_ZERO,
        .tag_encoding_width = 2,
        .flags_encoding_width = 2,
        .object_reference_width = 8,
        .invalid_tag = UINT16_MAX,
        .null_tag = 0,
        .object_reference_tag = 2,
        .valid_flags_mask = 3,
        .reserved_zero_mask = UINT16_MAX & ~UINT64_C(3),
        .tag_count = 3,
    };
    abi.fields[0] = make_field(XR_RUNTIME_FIELD_DYN_TAG, 0, 2, 2,
                               XR_RUNTIME_FIELD_UNSIGNED);
    abi.fields[1] = make_field(XR_RUNTIME_FIELD_DYN_FLAGS, 2, 2, 2,
                               XR_RUNTIME_FIELD_BITSET);
    abi.fields[2] = make_field(XR_RUNTIME_FIELD_DYN_PAYLOAD, 8, 8, 8,
                               XR_RUNTIME_FIELD_OPAQUE_BITS);
    for (size_t i = 0; i < abi.tag_count; i++) {
        abi.tags[i].stable_id = make_id((uint8_t) (20 + i));
        abi.tags[i].encoding = i;
        abi.tags[i].allowed_flags = abi.valid_flags_mask;
    }
    abi.tags[0].payload_kind = XR_RUNTIME_DYN_PAYLOAD_NONE;
    abi.tags[0].allowed_flags = 0;
    abi.tags[1].payload_kind = XR_RUNTIME_DYN_PAYLOAD_INLINE_BITS;
    abi.tags[2].payload_kind = XR_RUNTIME_DYN_PAYLOAD_OBJECT_REFERENCE;
    abi.tags[2].required_flags = 1;
    return abi;
}

static XrRuntimeEnumNamespaceAbi make_enum_namespace(uint16_t role, uint8_t width,
                                                     uint64_t invalid, size_t count,
                                                     uint8_t seed, uint64_t first_value) {
    XrRuntimeEnumNamespaceAbi space = {
        .invalid_encoding = invalid,
        .role = role,
        .entry_count = (uint16_t) count,
        .kind = XR_RUNTIME_NAMESPACE_ENUM,
        .encoding_width = width,
    };
    for (size_t i = 0; i < count; i++) {
        space.entries[i].stable_id = make_id((uint8_t) (seed + i));
        space.entries[i].encoding = first_value + i;
    }
    return space;
}

static XrRuntimeEnumNamespaceAbi make_bitmask_namespace(uint16_t role, uint8_t width,
                                                        size_t count, uint8_t seed) {
    XrRuntimeEnumNamespaceAbi space = {
        .role = role,
        .entry_count = (uint16_t) count,
        .kind = XR_RUNTIME_NAMESPACE_BITMASK,
        .encoding_width = width,
    };
    for (size_t i = 0; i < count; i++) {
        space.entries[i].stable_id = make_id((uint8_t) (seed + i));
        space.entries[i].encoding = UINT64_C(1) << i;
        space.valid_mask |= space.entries[i].encoding;
    }
    uint64_t complete = width == 8 ? UINT64_MAX : (UINT64_C(1) << (width * 8)) - 1;
    space.reserved_zero_mask = complete & ~space.valid_mask;
    return space;
}

static XrRuntimeEnumNamespaceAbi make_sentinel_namespace(uint16_t role, uint8_t width,
                                                         uint64_t sentinel) {
    XrRuntimeEnumNamespaceAbi space = {
        .invalid_encoding = sentinel,
        .role = role,
        .kind = XR_RUNTIME_NAMESPACE_SENTINEL,
        .encoding_width = width,
    };
    return space;
}

static void init_record(XrRuntimeRecordAbi *record, uint16_t kind, uint16_t size,
                        uint16_t alignment, uint16_t field_count,
                        uint16_t namespace_count) {
    *record = (XrRuntimeRecordAbi) {
        .schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .record_kind = kind,
        .size = size,
        .alignment = alignment,
        .field_count = field_count,
        .namespace_count = namespace_count,
    };
}

#define SET_RECORD_FIELD(record, index, struct_type, member, member_type, field_role,        \
                         field_encoding)                                                     \
    (record).fields[index] =                                                                 \
        make_field(field_role, (uint16_t) offsetof(struct_type, member),                     \
                   (uint16_t) sizeof(((struct_type *) 0)->member),                            \
                   (uint16_t) _Alignof(member_type), field_encoding)

static XrRuntimeRecordAbi make_domain_record(void) {
    XrRuntimeRecordAbi record;
    init_record(&record, XR_RUNTIME_RECORD_DOMAIN_IDENTITY,
                (uint16_t) sizeof(XrRuntimeDomainIdentity),
                (uint16_t) _Alignof(XrRuntimeDomainIdentity), 4, 2);
    SET_RECORD_FIELD(record, 0, XrRuntimeDomainIdentity, contract_id, XrStableId,
                     XR_RUNTIME_FIELD_DOMAIN_CONTRACT_ID, XR_RUNTIME_FIELD_STABLE_ID);
    SET_RECORD_FIELD(record, 1, XrRuntimeDomainIdentity, instance_id, uint32_t,
                     XR_RUNTIME_FIELD_DOMAIN_INSTANCE_ID, XR_RUNTIME_FIELD_UNSIGNED);
    SET_RECORD_FIELD(record, 2, XrRuntimeDomainIdentity, semantic_domain, uint8_t,
                     XR_RUNTIME_FIELD_DOMAIN_SEMANTIC, XR_RUNTIME_FIELD_UNSIGNED);
    SET_RECORD_FIELD(record, 3, XrRuntimeDomainIdentity, materialization, uint8_t,
                     XR_RUNTIME_FIELD_DOMAIN_MATERIALIZATION, XR_RUNTIME_FIELD_UNSIGNED);
    record.namespaces[0] = make_enum_namespace(
        XR_RUNTIME_NAMESPACE_SEMANTIC_DOMAIN, 1, XR_STORAGE_DOMAIN_UNKNOWN,
        XR_STORAGE_FOREIGN, 30, XR_STORAGE_EXEC_LOCAL);
    record.namespaces[1] = make_enum_namespace(
        XR_RUNTIME_NAMESPACE_MATERIALIZATION, 1, XR_MATERIALIZE_INVALID,
        XR_MATERIALIZE_EXTERNAL, 40, XR_MATERIALIZE_INLINE);
    return record;
}

static XrRuntimeRecordAbi make_extent_record(void) {
    XrRuntimeRecordAbi record;
    init_record(&record, XR_RUNTIME_RECORD_EXTENT_DESCRIPTOR,
                (uint16_t) sizeof(XrRuntimeExtentDescriptor),
                (uint16_t) _Alignof(XrRuntimeExtentDescriptor), 12, 2);
    SET_RECORD_FIELD(record, 0, XrRuntimeExtentDescriptor, schema_version, uint32_t,
                     XR_RUNTIME_FIELD_EXTENT_SCHEMA, XR_RUNTIME_FIELD_UNSIGNED);
    SET_RECORD_FIELD(record, 1, XrRuntimeExtentDescriptor, id, XrStableId,
                     XR_RUNTIME_FIELD_EXTENT_ID, XR_RUNTIME_FIELD_STABLE_ID);
    SET_RECORD_FIELD(record, 2, XrRuntimeExtentDescriptor, layout_id, XrStableId,
                     XR_RUNTIME_FIELD_EXTENT_LAYOUT_ID, XR_RUNTIME_FIELD_STABLE_ID);
    SET_RECORD_FIELD(record, 3, XrRuntimeExtentDescriptor, group_id, XrStableId,
                     XR_RUNTIME_FIELD_EXTENT_GROUP_ID, XR_RUNTIME_FIELD_STABLE_ID);
    SET_RECORD_FIELD(record, 4, XrRuntimeExtentDescriptor, provider_id, XrStableId,
                     XR_RUNTIME_FIELD_EXTENT_PROVIDER_ID, XR_RUNTIME_FIELD_STABLE_ID);
    SET_RECORD_FIELD(record, 5, XrRuntimeExtentDescriptor, tail_offset, uint64_t,
                     XR_RUNTIME_FIELD_EXTENT_TAIL_OFFSET, XR_RUNTIME_FIELD_UNSIGNED);
    SET_RECORD_FIELD(record, 6, XrRuntimeExtentDescriptor, stride, uint64_t,
                     XR_RUNTIME_FIELD_EXTENT_STRIDE, XR_RUNTIME_FIELD_UNSIGNED);
    SET_RECORD_FIELD(record, 7, XrRuntimeExtentDescriptor, operand_index, uint16_t,
                     XR_RUNTIME_FIELD_EXTENT_OPERAND_INDEX, XR_RUNTIME_FIELD_UNSIGNED);
    SET_RECORD_FIELD(record, 8, XrRuntimeExtentDescriptor, part_index, uint16_t,
                     XR_RUNTIME_FIELD_EXTENT_PART_INDEX, XR_RUNTIME_FIELD_UNSIGNED);
    SET_RECORD_FIELD(record, 9, XrRuntimeExtentDescriptor, part_count, uint16_t,
                     XR_RUNTIME_FIELD_EXTENT_PART_COUNT, XR_RUNTIME_FIELD_UNSIGNED);
    SET_RECORD_FIELD(record, 10, XrRuntimeExtentDescriptor, kind, uint8_t,
                     XR_RUNTIME_FIELD_EXTENT_KIND, XR_RUNTIME_FIELD_UNSIGNED);
    SET_RECORD_FIELD(record, 11, XrRuntimeExtentDescriptor, fingerprint, XrFingerprint,
                     XR_RUNTIME_FIELD_EXTENT_FINGERPRINT, XR_RUNTIME_FIELD_FINGERPRINT);
    record.namespaces[0] = make_enum_namespace(XR_RUNTIME_NAMESPACE_EXTENT_KIND, 1,
                                               UINT8_MAX, XR_RUNTIME_EXTENT_KIND_COUNT, 50,
                                               XR_RUNTIME_EXTENT_FIXED);
    record.namespaces[1] = make_sentinel_namespace(
        XR_RUNTIME_NAMESPACE_EXTENT_OPERAND, 2, XR_RUNTIME_EXTENT_OPERAND_NONE);
    return record;
}

static XrRuntimeRecordAbi make_layout_record(void) {
    XrRuntimeRecordAbi record;
    init_record(&record, XR_RUNTIME_RECORD_LAYOUT_DESCRIPTOR,
                (uint16_t) sizeof(XrRuntimeLayoutDescriptor),
                (uint16_t) _Alignof(XrRuntimeLayoutDescriptor), 16, 1);
    SET_RECORD_FIELD(record, 0, XrRuntimeLayoutDescriptor, schema_version, uint32_t,
                     XR_RUNTIME_FIELD_LAYOUT_SCHEMA, XR_RUNTIME_FIELD_UNSIGNED);
    SET_RECORD_FIELD(record, 1, XrRuntimeLayoutDescriptor, descriptor_id, XrStableId,
                     XR_RUNTIME_FIELD_LAYOUT_DESCRIPTOR_ID, XR_RUNTIME_FIELD_STABLE_ID);
    SET_RECORD_FIELD(record, 2, XrRuntimeLayoutDescriptor, layout_id, XrStableId,
                     XR_RUNTIME_FIELD_LAYOUT_ID, XR_RUNTIME_FIELD_STABLE_ID);
    SET_RECORD_FIELD(record, 3, XrRuntimeLayoutDescriptor, object_kind_id, XrStableId,
                     XR_RUNTIME_FIELD_LAYOUT_OBJECT_KIND_ID, XR_RUNTIME_FIELD_STABLE_ID);
    SET_RECORD_FIELD(record, 4, XrRuntimeLayoutDescriptor, extent_id, XrStableId,
                     XR_RUNTIME_FIELD_LAYOUT_EXTENT_ID, XR_RUNTIME_FIELD_STABLE_ID);
    SET_RECORD_FIELD(record, 5, XrRuntimeLayoutDescriptor, root_plan_id, XrStableId,
                     XR_RUNTIME_FIELD_LAYOUT_ROOT_PLAN_ID, XR_RUNTIME_FIELD_STABLE_ID);
    SET_RECORD_FIELD(record, 6, XrRuntimeLayoutDescriptor, destructor_id, XrStableId,
                     XR_RUNTIME_FIELD_LAYOUT_DESTRUCTOR_ID, XR_RUNTIME_FIELD_STABLE_ID);
    SET_RECORD_FIELD(record, 7, XrRuntimeLayoutDescriptor, clone_id, XrStableId,
                     XR_RUNTIME_FIELD_LAYOUT_CLONE_ID, XR_RUNTIME_FIELD_STABLE_ID);
    SET_RECORD_FIELD(record, 8, XrRuntimeLayoutDescriptor, eq_hash_id, XrStableId,
                     XR_RUNTIME_FIELD_LAYOUT_EQ_HASH_ID, XR_RUNTIME_FIELD_STABLE_ID);
    SET_RECORD_FIELD(record, 9, XrRuntimeLayoutDescriptor, extent_fingerprint,
                     XrFingerprint, XR_RUNTIME_FIELD_LAYOUT_EXTENT_FINGERPRINT,
                     XR_RUNTIME_FIELD_FINGERPRINT);
    SET_RECORD_FIELD(record, 10, XrRuntimeLayoutDescriptor, fixed_prefix_size, uint64_t,
                     XR_RUNTIME_FIELD_LAYOUT_FIXED_PREFIX_SIZE, XR_RUNTIME_FIELD_UNSIGNED);
    SET_RECORD_FIELD(record, 11, XrRuntimeLayoutDescriptor, alignment, uint32_t,
                     XR_RUNTIME_FIELD_LAYOUT_ALIGNMENT, XR_RUNTIME_FIELD_UNSIGNED);
    SET_RECORD_FIELD(record, 12, XrRuntimeLayoutDescriptor, allowed_semantic_domains,
                     uint32_t, XR_RUNTIME_FIELD_LAYOUT_SEMANTIC_DOMAINS,
                     XR_RUNTIME_FIELD_BITSET);
    SET_RECORD_FIELD(record, 13, XrRuntimeLayoutDescriptor, allowed_materializations,
                     uint32_t, XR_RUNTIME_FIELD_LAYOUT_MATERIALIZATIONS,
                     XR_RUNTIME_FIELD_BITSET);
    SET_RECORD_FIELD(record, 14, XrRuntimeLayoutDescriptor, flags, uint32_t,
                     XR_RUNTIME_FIELD_LAYOUT_FLAGS, XR_RUNTIME_FIELD_BITSET);
    SET_RECORD_FIELD(record, 15, XrRuntimeLayoutDescriptor, fingerprint, XrFingerprint,
                     XR_RUNTIME_FIELD_LAYOUT_FINGERPRINT, XR_RUNTIME_FIELD_FINGERPRINT);
    record.namespaces[0] = make_bitmask_namespace(
        XR_RUNTIME_NAMESPACE_LAYOUT_FLAGS, 4, 4, 60);
    return record;
}

static XrRuntimeRecordAbi make_limits_record(void) {
    XrRuntimeRecordAbi record;
    init_record(&record, XR_RUNTIME_RECORD_EXTENT_LIMITS,
                (uint16_t) sizeof(XrRuntimeExtentLimits),
                (uint16_t) _Alignof(XrRuntimeExtentLimits), 2, 0);
    SET_RECORD_FIELD(record, 0, XrRuntimeExtentLimits, max_allocation_bytes, uint64_t,
                     XR_RUNTIME_FIELD_LIMIT_MAX_ALLOCATION_BYTES,
                     XR_RUNTIME_FIELD_UNSIGNED);
    SET_RECORD_FIELD(record, 1, XrRuntimeExtentLimits, max_alignment, uint32_t,
                     XR_RUNTIME_FIELD_LIMIT_MAX_ALIGNMENT, XR_RUNTIME_FIELD_UNSIGNED);
    return record;
}

static XrRuntimeRecordAbi make_evaluated_record(void) {
    XrRuntimeRecordAbi record;
    init_record(&record, XR_RUNTIME_RECORD_EVALUATED_EXTENT,
                (uint16_t) sizeof(XrRuntimeEvaluatedExtent),
                (uint16_t) _Alignof(XrRuntimeEvaluatedExtent), 7, 0);
    SET_RECORD_FIELD(record, 0, XrRuntimeEvaluatedExtent, extent_id, XrStableId,
                     XR_RUNTIME_FIELD_EVALUATED_EXTENT_ID, XR_RUNTIME_FIELD_STABLE_ID);
    SET_RECORD_FIELD(record, 1, XrRuntimeEvaluatedExtent, extent_fingerprint,
                     XrFingerprint, XR_RUNTIME_FIELD_EVALUATED_EXTENT_FINGERPRINT,
                     XR_RUNTIME_FIELD_FINGERPRINT);
    SET_RECORD_FIELD(record, 2, XrRuntimeEvaluatedExtent, bytes, uint64_t,
                     XR_RUNTIME_FIELD_EVALUATED_BYTES, XR_RUNTIME_FIELD_UNSIGNED);
    SET_RECORD_FIELD(record, 3, XrRuntimeEvaluatedExtent, operand, uint64_t,
                     XR_RUNTIME_FIELD_EVALUATED_OPERAND, XR_RUNTIME_FIELD_UNSIGNED);
    SET_RECORD_FIELD(record, 4, XrRuntimeEvaluatedExtent, alignment, uint32_t,
                     XR_RUNTIME_FIELD_EVALUATED_ALIGNMENT, XR_RUNTIME_FIELD_UNSIGNED);
    SET_RECORD_FIELD(record, 5, XrRuntimeEvaluatedExtent, part_index, uint16_t,
                     XR_RUNTIME_FIELD_EVALUATED_PART_INDEX, XR_RUNTIME_FIELD_UNSIGNED);
    SET_RECORD_FIELD(record, 6, XrRuntimeEvaluatedExtent, part_count, uint16_t,
                     XR_RUNTIME_FIELD_EVALUATED_PART_COUNT, XR_RUNTIME_FIELD_UNSIGNED);
    return record;
}

static XrRuntimeRecordAbi make_group_record(void) {
    XrRuntimeRecordAbi record;
    init_record(&record, XR_RUNTIME_RECORD_EXTENT_GROUP_SUMMARY,
                (uint16_t) sizeof(XrRuntimeExtentGroupSummary),
                (uint16_t) _Alignof(XrRuntimeExtentGroupSummary), 3, 0);
    SET_RECORD_FIELD(record, 0, XrRuntimeExtentGroupSummary, group_id, XrStableId,
                     XR_RUNTIME_FIELD_GROUP_ID, XR_RUNTIME_FIELD_STABLE_ID);
    SET_RECORD_FIELD(record, 1, XrRuntimeExtentGroupSummary, part_count, uint16_t,
                     XR_RUNTIME_FIELD_GROUP_PART_COUNT, XR_RUNTIME_FIELD_UNSIGNED);
    SET_RECORD_FIELD(record, 2, XrRuntimeExtentGroupSummary, fingerprint, XrFingerprint,
                     XR_RUNTIME_FIELD_GROUP_FINGERPRINT, XR_RUNTIME_FIELD_FINGERPRINT);
    return record;
}

static XrRuntimeExtentProviderCallbackAbi make_callback_abi(void) {
    XrRuntimeExtentProviderCallbackAbi callback = {
        .schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .contract_id = {{70}},
        .provider_id_width = XR_STABLE_ID_BYTES,
        .operand_element_width = sizeof(uint64_t),
        .operand_count_width = sizeof(size_t),
        .result_width = sizeof(uint64_t),
        .error_normalization = XR_RUNTIME_PROVIDER_ERROR_NON_OK_TO_REJECTED,
    };
    callback.contract_id = make_id(70);
    callback.status_namespace = make_enum_namespace(
        XR_RUNTIME_NAMESPACE_STATUS, 1, UINT8_MAX, XR_RUNTIME_ABI_STATUS_COUNT, 80,
        XR_RUNTIME_ABI_OK);
    return callback;
}

static XrRuntimeAbiContract make_runtime_abi(void) {
    XrRuntimeAbiContract abi = {
        .schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .stable_id_width = XR_STABLE_ID_BYTES,
        .fingerprint_width = XR_FINGERPRINT_BYTES,
        .pointer_width = sizeof(void *),
        .canonical_serialization_endian = XR_RUNTIME_ENDIAN_LITTLE,
        .target_endian = XR_RUNTIME_ENDIAN_LITTLE,
        .checked_arithmetic_policy = XR_RUNTIME_CHECKED_ARITHMETIC_REJECT_OVERFLOW,
        .alignment_policy = XR_RUNTIME_ALIGNMENT_POWER_OF_TWO_REJECT_OVERFLOW,
        .unknown_enum_policy = XR_RUNTIME_UNKNOWN_ENUM_REJECT,
        .reserved_zero_policy = XR_RUNTIME_RESERVED_ZERO_REJECT,
    };
    abi.object_header = make_object_header_abi();
    abi.dynamic_value = make_dynamic_value_abi();
    abi.dynamic_value.object_reference_width = (uint8_t) sizeof(void *);
    abi.domain_identity = make_domain_record();
    abi.extent_descriptor = make_extent_record();
    abi.layout_descriptor = make_layout_record();
    abi.extent_limits = make_limits_record();
    abi.evaluated_extent = make_evaluated_record();
    abi.extent_group_summary = make_group_record();
    abi.extent_provider_callback = make_callback_abi();
    return abi;
}

static XrTargetProviderOperationContract make_operation(uint8_t id_seed, uint8_t fp_seed,
                                                        uint32_t effects,
                                                        uint32_t lifetime,
                                                        uint32_t failures) {
    XrTargetProviderOperationContract operation = {
        .stable_id = {{0}},
        .call_abi_fingerprint = {{0}},
        .effect_flags = effects,
        .lifetime_flags = lifetime,
        .failure_flags = failures,
    };
    operation.stable_id = make_id(id_seed);
    operation.call_abi_fingerprint = make_fingerprint(fp_seed);
    return operation;
}

static void make_providers(XrTargetProviderContract providers[2]) {
    memset(providers, 0, 2 * sizeof(providers[0]));
    providers[0] = (XrTargetProviderContract) {
        .schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .contract_id = {{0}},
        .abi_schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .flags = XR_TARGET_PROVIDER_AVAILABLE_HOSTED,
        .operation_count = 2,
        .runtime_profile = XR_TARGET_RUNTIME_PROFILE_HOSTED,
        .provider_kind = XR_TARGET_PROVIDER_ALLOCATOR,
        .allocator_max_alignment = 64,
        .allocator_sized_free = 1,
        .allocator_zeroed_allocation = 1,
        .allocator_thread_safe = 1,
    };
    providers[0].contract_id = make_id(200);
    providers[0].operations[0] = make_operation(
        10, 1, XR_TARGET_PROVIDER_EFFECT_ALLOCATES,
        XR_TARGET_PROVIDER_LIFETIME_RETURNS_OWNED,
        XR_TARGET_PROVIDER_FAILURE_RETURNS_STATUS);
    providers[0].operations[1] = make_operation(
        11, 2, XR_TARGET_PROVIDER_EFFECT_DEALLOCATES,
        XR_TARGET_PROVIDER_LIFETIME_CONSUMES_OWNED, 0);

    providers[1] = (XrTargetProviderContract) {
        .schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .contract_id = {{0}},
        .abi_schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .flags = XR_TARGET_PROVIDER_AVAILABLE_HOSTED,
        .operation_count = 1,
        .runtime_profile = XR_TARGET_RUNTIME_PROFILE_HOSTED,
        .provider_kind = XR_TARGET_PROVIDER_PANIC,
        .panic_behavior = XR_TARGET_PROVIDER_PANIC_NO_RETURN,
    };
    providers[1].contract_id = make_id(201);
    providers[1].operations[0] = make_operation(
        20, 3, XR_TARGET_PROVIDER_EFFECT_PANICS, 0,
        XR_TARGET_PROVIDER_FAILURE_NO_RETURN);
}

static void test_object_header_known_answer_and_mutation(void) {
    XrRuntimeObjectHeaderAbi abi = make_object_header_abi();
    XrFingerprint fingerprint;
    CHECK(xr_runtime_object_header_abi_fingerprint(&abi, &fingerprint) ==
              XR_RUNTIME_ABI_OK,
          "canonical object-header ABI fingerprints");
    static const uint8_t expected[XR_FINGERPRINT_BYTES] = {
        0x17, 0x84, 0x08, 0xb9, 0x94, 0x34, 0xc6, 0xc9,
        0x74, 0x43, 0x78, 0x7a, 0xb9, 0x1c, 0xe5, 0x2d,
        0x5f, 0xe5, 0x8a, 0xf7, 0xf8, 0x81, 0x70, 0x75,
        0xa4, 0xc8, 0xcd, 0x6b, 0x1f, 0xd2, 0x4c, 0x18,
    };
    if (memcmp(fingerprint.bytes, expected, sizeof(expected)) != 0)
        print_fingerprint("object-header fingerprint", fingerprint);
    CHECK(memcmp(fingerprint.bytes, expected, sizeof(expected)) == 0,
          "object-header fingerprint matches the frozen known answer");

    XrRuntimeObjectHeaderAbi mutated = abi;
    mutated.object_kinds.entries[2].encoding = 4;
    XrFingerprint changed;
    CHECK(xr_runtime_object_header_abi_fingerprint(&mutated, &changed) ==
                  XR_RUNTIME_ABI_OK &&
              !fingerprint_equal(fingerprint, changed),
          "valid object-kind registry mutation changes the fingerprint");

    XrFingerprint untouched = make_fingerprint(230);
    changed = untouched;
    mutated = abi;
    mutated.fields[2].offset = 4;
    CHECK(xr_runtime_object_header_abi_fingerprint(&mutated, &changed) ==
                  XR_RUNTIME_ABI_INVALID_OVERLAP &&
              fingerprint_equal(changed, untouched),
          "overlapping header fields fail without publishing a digest");
    mutated = abi;
    uint16_t role = mutated.fields[1].role;
    mutated.fields[1].role = mutated.fields[2].role;
    mutated.fields[2].role = role;
    CHECK(xr_runtime_object_header_abi_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_ORDER,
          "header semantic field order is exact");
    mutated = abi;
    mutated.object_kinds.entries[1].stable_id = make_id(1);
    CHECK(xr_runtime_object_header_abi_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_ORDER,
          "object-kind stable IDs are strictly ordered");
    mutated = abi;
    mutated.object_kinds.entries[1].encoding =
        mutated.object_kinds.entries[0].encoding;
    CHECK(xr_runtime_object_header_abi_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_ORDER,
          "object-kind numeric encodings are unique");
    mutated = abi;
    mutated.object_kinds.entry_count = XR_RUNTIME_ABI_MAX_OBJECT_KINDS + 1;
    CHECK(xr_runtime_object_header_abi_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_BUDGET_EXCEEDED,
          "object-kind registry budget is fail closed");
    mutated = abi;
    memset(&mutated.object_kinds.entries[0].stable_id, 0,
           sizeof(mutated.object_kinds.entries[0].stable_id));
    CHECK(xr_runtime_object_header_abi_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_SHAPE,
          "zero object-kind stable IDs are rejected");
    mutated = abi;
    mutated.object_kinds.entries[3].encoding = 4;
    CHECK(xr_runtime_object_header_abi_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_POLICY,
          "unused object-kind registry rows must be zero");
    mutated = abi;
    mutated.flags.reserved_zero_mask ^= UINT64_C(8);
    CHECK(xr_runtime_object_header_abi_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_MASK,
          "header flag valid and reserved masks must be complete");
    mutated = abi;
    mutated.flags.entries[1].exclusivity_group = 0;
    CHECK(xr_runtime_object_header_abi_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_SHAPE,
          "flag exclusivity groups cannot contain one member");
    mutated = abi;
    mutated.flags.entry_count = XR_RUNTIME_ABI_MAX_OBJECT_FLAGS + 1;
    CHECK(xr_runtime_object_header_abi_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_BUDGET_EXCEEDED,
          "object-flag registry budget is fail closed");
    mutated = abi;
    mutated.flags.entries[0].bit = 0;
    CHECK(xr_runtime_object_header_abi_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_SHAPE,
          "zero object-flag bits are rejected");
    mutated = abi;
    mutated.flags.entries[0].bit = 3;
    CHECK(xr_runtime_object_header_abi_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_SHAPE,
          "multi-bit object-flag entries are rejected");
    mutated = abi;
    mutated.flags.entries[1].stable_id = mutated.flags.entries[0].stable_id;
    CHECK(xr_runtime_object_header_abi_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_ORDER,
          "object-flag stable IDs are unique and ordered");
    mutated = abi;
    mutated.flags.entries[1].bit = mutated.flags.entries[0].bit;
    CHECK(xr_runtime_object_header_abi_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_SHAPE,
          "object-flag bits are unique");
    mutated = abi;
    mutated.flags.entries[3].reserved32 = 1;
    CHECK(xr_runtime_object_header_abi_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_POLICY,
          "unused object-flag registry rows must be zero");
    mutated = abi;
    mutated.reserved32 = 1;
    CHECK(xr_runtime_object_header_abi_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_POLICY,
          "object-header reserved words must be zero");
    mutated = abi;
    mutated.rc.shared_release_order = XR_RUNTIME_MEMORY_ORDER_RELAXED;
    CHECK(xr_runtime_object_header_abi_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_POLICY,
          "unsupported RC memory-order policy is rejected");
}

static void test_runtime_known_answer_and_mutation(void) {
    XrRuntimeAbiContract abi = make_runtime_abi();
    XrFingerprint fingerprint;
    CHECK(xr_runtime_abi_contract_fingerprint(&abi, &fingerprint) == XR_RUNTIME_ABI_OK,
          "whole runtime ABI fingerprints");
    static const uint8_t expected[XR_FINGERPRINT_BYTES] = {
        0x8d, 0x92, 0xd1, 0x65, 0xa3, 0xb3, 0x5f, 0x34,
        0xaf, 0x94, 0xf5, 0xa4, 0xf7, 0xff, 0xd3, 0xb9,
        0xf1, 0x79, 0x67, 0x99, 0xb2, 0x0d, 0x0e, 0xac,
        0xe5, 0x87, 0x39, 0x33, 0x98, 0x72, 0xf1, 0x98,
    };
    if (memcmp(fingerprint.bytes, expected, sizeof(expected)) != 0)
        print_fingerprint("runtime ABI fingerprint", fingerprint);
    CHECK(memcmp(fingerprint.bytes, expected, sizeof(expected)) == 0,
          "runtime ABI fingerprint matches the frozen known answer");

    XrRuntimeAbiContract mutated = abi;
    mutated.dynamic_value.tags[1].required_flags = 2;
    XrFingerprint changed;
    CHECK(xr_runtime_abi_contract_fingerprint(&mutated, &changed) == XR_RUNTIME_ABI_OK &&
              !fingerprint_equal(fingerprint, changed),
          "valid dynamic-tag mutation changes the whole-runtime fingerprint");

    XrFingerprint untouched = make_fingerprint(231);
    changed = untouched;
    mutated = abi;
    mutated.dynamic_value.fields[1].offset = 0;
    CHECK(xr_runtime_abi_contract_fingerprint(&mutated, &changed) ==
                  XR_RUNTIME_ABI_INVALID_OVERLAP &&
              fingerprint_equal(changed, untouched),
          "dynamic-value overlap fails without publishing a digest");
    mutated = abi;
    mutated.dynamic_value.tags[1].stable_id = make_id(19);
    CHECK(xr_runtime_abi_contract_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_ORDER,
          "dynamic tag registry is stable-ID sorted");
    mutated = abi;
    mutated.dynamic_value.tags[1].encoding = mutated.dynamic_value.tags[0].encoding;
    CHECK(xr_runtime_abi_contract_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_ORDER,
          "dynamic tag encodings are unique");
    mutated = abi;
    mutated.dynamic_value.tags[0].allowed_flags = 1;
    CHECK(xr_runtime_abi_contract_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_POLICY,
          "canonical null tag rejects nonzero flags");
    mutated = abi;
    mutated.dynamic_value.reserved_zero_mask ^= UINT64_C(4);
    CHECK(xr_runtime_abi_contract_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_MASK,
          "dynamic flag masks cover the full namespace exactly");
    mutated = abi;
    mutated.dynamic_value.tag_count = XR_RUNTIME_ABI_MAX_DYNAMIC_TAGS + 1;
    CHECK(xr_runtime_abi_contract_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_BUDGET_EXCEEDED,
          "dynamic tag count obeys the hard budget");
    mutated = abi;
    mutated.dynamic_value.tags[3].stable_id = make_id(23);
    CHECK(xr_runtime_abi_contract_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_POLICY,
          "unused dynamic registry slots must be zero");
    mutated = abi;
    mutated.pointer_width = 16;
    CHECK(xr_runtime_abi_contract_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_SHAPE,
          "unsupported pointer width is rejected");
    mutated = abi;
    mutated.extent_descriptor.field_count = XR_RUNTIME_ABI_MAX_RECORD_FIELDS + 1;
    CHECK(xr_runtime_abi_contract_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_BUDGET_EXCEEDED,
          "runtime record field count obeys the hard budget");
    mutated = abi;
    mutated.extent_descriptor.namespaces[0].entry_count =
        XR_RUNTIME_ABI_MAX_ENUM_VALUES + 1;
    CHECK(xr_runtime_abi_contract_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_BUDGET_EXCEEDED,
          "runtime enum namespace obeys the hard budget");
    mutated = abi;
    mutated.domain_identity.namespaces[0].role =
        XR_RUNTIME_NAMESPACE_MATERIALIZATION;
    CHECK(xr_runtime_abi_contract_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_SHAPE,
          "runtime enum namespace order and role are exact");
    mutated = abi;
    mutated.layout_descriptor.namespaces[0].reserved_zero_mask ^= UINT64_C(16);
    CHECK(xr_runtime_abi_contract_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_MASK,
          "record bitmask namespace requires an exact reserved mask");
    mutated = abi;
    uint16_t field_role = mutated.extent_descriptor.fields[0].role;
    mutated.extent_descriptor.fields[0].role =
        mutated.extent_descriptor.fields[1].role;
    mutated.extent_descriptor.fields[1].role = field_role;
    CHECK(xr_runtime_abi_contract_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_ORDER,
          "runtime record semantic field order is exact");
    mutated = abi;
    mutated.extent_limits.fields[2] = make_field(
        XR_RUNTIME_FIELD_LIMIT_MAX_ALIGNMENT, 12, 4, 4, XR_RUNTIME_FIELD_UNSIGNED);
    CHECK(xr_runtime_abi_contract_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_POLICY,
          "unused runtime record field slots must be zero");
    mutated = abi;
    mutated.extent_provider_callback.status_namespace.entry_count--;
    memset(&mutated.extent_provider_callback.status_namespace.entries[
               mutated.extent_provider_callback.status_namespace.entry_count],
           0, sizeof(mutated.extent_provider_callback.status_namespace.entries[0]));
    CHECK(xr_runtime_abi_contract_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_SHAPE,
          "provider callback status namespace must be complete");
    mutated = abi;
    mutated.extent_provider_callback.reserved32 = 1;
    CHECK(xr_runtime_abi_contract_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_POLICY,
          "provider callback reserved words must be zero");
    mutated = abi;
    mutated.checked_arithmetic_policy = XR_RUNTIME_CHECKED_ARITHMETIC_INVALID;
    CHECK(xr_runtime_abi_contract_fingerprint(&mutated, &changed) ==
              XR_RUNTIME_ABI_INVALID_POLICY,
          "unknown checked-arithmetic policy is rejected");
}

static void test_provider_set_known_answer_and_mutation(void) {
    XrTargetProviderContract providers[2];
    make_providers(providers);
    uint64_t provider_mask = 0;
    XrFingerprint fingerprint;
    CHECK(xr_target_provider_set_fingerprint(providers, 2, &provider_mask, &fingerprint) ==
              XR_RUNTIME_ABI_OK,
          "structured provider set fingerprints");
    uint64_t expected_mask = XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_ALLOCATOR) |
                             XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_PANIC);
    CHECK(provider_mask == expected_mask,
          "provider mask is derived exactly from verified provider kinds");
    static const uint8_t expected[XR_FINGERPRINT_BYTES] = {
        0x13, 0xe0, 0x62, 0xa1, 0x30, 0xd9, 0x65, 0x70,
        0xc5, 0xa6, 0x46, 0x68, 0x63, 0xa7, 0x0f, 0x4a,
        0xe3, 0x4e, 0x89, 0x0d, 0xfd, 0xad, 0x20, 0x0b,
        0x98, 0xfc, 0xb4, 0xa4, 0xac, 0x89, 0x42, 0x71,
    };
    if (memcmp(fingerprint.bytes, expected, sizeof(expected)) != 0)
        print_fingerprint("provider-set fingerprint", fingerprint);
    CHECK(memcmp(fingerprint.bytes, expected, sizeof(expected)) == 0,
          "provider-set fingerprint matches the frozen known answer");

    XrTargetProviderContract freestanding[2];
    memcpy(freestanding, providers, sizeof(freestanding));
    for (size_t i = 0; i < 2; i++) {
        freestanding[i].runtime_profile = XR_TARGET_RUNTIME_PROFILE_FREESTANDING;
        freestanding[i].flags = XR_TARGET_PROVIDER_AVAILABLE_FREESTANDING;
    }
    uint64_t freestanding_mask = 0;
    XrFingerprint freestanding_fingerprint;
    CHECK(xr_target_provider_set_fingerprint(freestanding, 2, &freestanding_mask,
                                             &freestanding_fingerprint) ==
                  XR_RUNTIME_ABI_OK &&
              freestanding_mask == expected_mask &&
              !fingerprint_equal(fingerprint, freestanding_fingerprint),
          "freestanding mandatory providers form a distinct valid contract");

    XrTargetProviderContract mutated[2];
    memcpy(mutated, providers, sizeof(mutated));
    mutated[0].operations[0].call_abi_fingerprint = make_fingerprint(9);
    XrFingerprint changed;
    uint64_t changed_mask = 0;
    CHECK(xr_target_provider_set_fingerprint(mutated, 2, &changed_mask, &changed) ==
                  XR_RUNTIME_ABI_OK &&
              changed_mask == expected_mask && !fingerprint_equal(fingerprint, changed),
          "valid operation ABI mutation changes only the provider-set fingerprint");

    XrFingerprint untouched = make_fingerprint(232);
    uint64_t untouched_mask = UINT64_C(0xfeed);
    changed = untouched;
    changed_mask = untouched_mask;
    XrTargetProviderContract reversed[2] = {providers[1], providers[0]};
    CHECK(xr_target_provider_set_fingerprint(reversed, 2, &changed_mask, &changed) ==
                  XR_RUNTIME_ABI_INVALID_ORDER &&
              changed_mask == untouched_mask && fingerprint_equal(changed, untouched),
          "provider kind order fails without publishing mask or digest");
    CHECK(xr_target_provider_set_fingerprint(providers, 1, &changed_mask, &changed) ==
              XR_RUNTIME_ABI_INVALID_PROVIDER_SET,
          "missing mandatory panic provider is rejected");
    CHECK(xr_target_provider_set_fingerprint(&providers[1], 1, &changed_mask, &changed) ==
              XR_RUNTIME_ABI_INVALID_PROVIDER_SET,
          "missing mandatory allocator provider is rejected");
    CHECK(xr_target_provider_set_fingerprint(
              providers, XR_RUNTIME_ABI_MAX_PROVIDERS + 1, &changed_mask, &changed) ==
              XR_RUNTIME_ABI_BUDGET_EXCEEDED,
          "provider set count obeys the hard budget before reading entries");

    memcpy(mutated, providers, sizeof(mutated));
    mutated[0].operation_count = XR_RUNTIME_ABI_MAX_PROVIDER_OPERATIONS + 1;
    CHECK(xr_target_provider_set_fingerprint(mutated, 2, &changed_mask, &changed) ==
              XR_RUNTIME_ABI_BUDGET_EXCEEDED,
          "provider operation count obeys the hard budget");
    memcpy(mutated, providers, sizeof(mutated));
    mutated[0].operations[1].stable_id = make_id(9);
    CHECK(xr_target_provider_set_fingerprint(mutated, 2, &changed_mask, &changed) ==
              XR_RUNTIME_ABI_INVALID_ORDER,
          "provider operations are stable-ID sorted");
    memcpy(mutated, providers, sizeof(mutated));
    memset(&mutated[0].operations[0].call_abi_fingerprint, 0,
           sizeof(mutated[0].operations[0].call_abi_fingerprint));
    CHECK(xr_target_provider_set_fingerprint(mutated, 2, &changed_mask, &changed) ==
              XR_RUNTIME_ABI_INVALID_PROVIDER_SET,
          "zero operation call ABI is rejected");
    memcpy(mutated, providers, sizeof(mutated));
    mutated[0].operations[2].reserved64 = 1;
    CHECK(xr_target_provider_set_fingerprint(mutated, 2, &changed_mask, &changed) ==
              XR_RUNTIME_ABI_INVALID_POLICY,
          "unused provider operation slots must be zero");
    memcpy(mutated, providers, sizeof(mutated));
    mutated[0].operations[0].effect_flags |= UINT32_C(1) << 31;
    CHECK(xr_target_provider_set_fingerprint(mutated, 2, &changed_mask, &changed) ==
              XR_RUNTIME_ABI_INVALID_PROVIDER_SET,
          "unknown provider effect bits are rejected");
    memcpy(mutated, providers, sizeof(mutated));
    mutated[0].allocator_max_alignment = 3;
    CHECK(xr_target_provider_set_fingerprint(mutated, 2, &changed_mask, &changed) ==
              XR_RUNTIME_ABI_INVALID_PROVIDER_SET,
          "allocator alignment fact must be a power of two");
    memcpy(mutated, providers, sizeof(mutated));
    mutated[1].panic_behavior = XR_TARGET_PROVIDER_PANIC_UNWINDS;
    CHECK(xr_target_provider_set_fingerprint(mutated, 2, &changed_mask, &changed) ==
              XR_RUNTIME_ABI_INVALID_PROVIDER_SET,
          "panic behavior must agree with operation failure facts");
    memcpy(mutated, providers, sizeof(mutated));
    mutated[1].flags = XR_TARGET_PROVIDER_AVAILABLE_FREESTANDING;
    CHECK(xr_target_provider_set_fingerprint(mutated, 2, &changed_mask, &changed) ==
              XR_RUNTIME_ABI_INVALID_PROVIDER_SET,
          "provider availability must include the selected runtime profile");
    memcpy(mutated, providers, sizeof(mutated));
    mutated[1].provider_kind = XR_TARGET_PROVIDER_KIND_COUNT;
    CHECK(xr_target_provider_set_fingerprint(mutated, 2, &changed_mask, &changed) ==
              XR_RUNTIME_ABI_INVALID_PROVIDER_SET,
          "unknown provider kind is rejected");
    memcpy(mutated, providers, sizeof(mutated));
    mutated[1].reserved32 = 1;
    CHECK(xr_target_provider_set_fingerprint(mutated, 2, &changed_mask, &changed) ==
              XR_RUNTIME_ABI_INVALID_POLICY,
          "provider reserved fields must be zero");
}

int main(void) {
    test_object_header_known_answer_and_mutation();
    test_runtime_known_answer_and_mutation();
    test_provider_set_known_answer_and_mutation();
    if (failures != 0) {
        fprintf(stderr, "%d runtime ABI contract test(s) failed\n", failures);
        return 1;
    }
    puts("runtime ABI contract tests passed");
    return 0;
}
