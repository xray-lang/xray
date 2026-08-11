/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_runtime_target_authority.c - Native hosted runtime ABI authority
 */

#include "xr_runtime_target_authority.h"
#include "../value/xvalue.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct XrCanonicalEncoding {
    const char *key;
    uint64_t encoding;
} XrCanonicalEncoding;

typedef struct XrCanonicalField {
    uint16_t role;
    uint16_t offset;
    uint16_t width;
    uint16_t alignment;
    uint8_t encoding;
} XrCanonicalField;

static bool canonical_id(const char *key, XrStableId *out) {
    XrFingerprint digest;
    return xr_stable_id_from_key(key, out, &digest);
}

static int compare_stable_id_first(const void *left, const void *right) {
    return xr_stable_id_compare(*(const XrStableId *) left,
                                *(const XrStableId *) right);
}

static void set_field(XrRuntimePhysicalFieldAbi *field,
                      const XrCanonicalField *source) {
    *field = (XrRuntimePhysicalFieldAbi) {
        .role = source->role,
        .offset = source->offset,
        .width = source->width,
        .alignment = source->alignment,
        .encoding = source->encoding,
        .atomicity = XR_RUNTIME_FIELD_PLAIN,
        .index_semantics = XR_RUNTIME_INDEX_NONE,
    };
}

static bool make_namespace(XrRuntimeEnumNamespaceAbi *out, uint16_t role,
                           uint8_t kind, uint8_t width, uint64_t invalid,
                           const XrCanonicalEncoding *entries, size_t count) {
    if (!out || count > XR_RUNTIME_ABI_MAX_ENUM_VALUES)
        return false;
    *out = (XrRuntimeEnumNamespaceAbi) {
        .invalid_encoding = invalid,
        .role = role,
        .entry_count = (uint16_t) count,
        .kind = kind,
        .encoding_width = width,
    };
    for (size_t i = 0; i < count; i++) {
        if (!canonical_id(entries[i].key, &out->entries[i].stable_id))
            return false;
        out->entries[i].encoding = entries[i].encoding;
        if (kind == XR_RUNTIME_NAMESPACE_BITMASK)
            out->valid_mask |= entries[i].encoding;
    }
    if (kind == XR_RUNTIME_NAMESPACE_BITMASK) {
        uint64_t complete = width == 8
                                ? UINT64_MAX
                                : (UINT64_C(1) << (width * 8)) - UINT64_C(1);
        out->reserved_zero_mask = complete & ~out->valid_mask;
    }
    qsort(out->entries, count, sizeof(out->entries[0]), compare_stable_id_first);
    return true;
}

static void make_sentinel_namespace(XrRuntimeEnumNamespaceAbi *out,
                                    uint16_t role, uint8_t width,
                                    uint64_t sentinel) {
    *out = (XrRuntimeEnumNamespaceAbi) {
        .invalid_encoding = sentinel,
        .role = role,
        .kind = XR_RUNTIME_NAMESPACE_SENTINEL,
        .encoding_width = width,
    };
}

static void make_record(XrRuntimeRecordAbi *out, uint16_t kind, uint16_t size,
                        uint16_t alignment, const XrCanonicalField *fields,
                        size_t field_count) {
    *out = (XrRuntimeRecordAbi) {
        .schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .record_kind = kind,
        .size = size,
        .alignment = alignment,
        .field_count = (uint16_t) field_count,
    };
    for (size_t i = 0; i < field_count; i++)
        set_field(&out->fields[i], &fields[i]);
}

#define XR_AUTHORITY_FIELD(type, member, member_type, field_role, field_encoding)        \
    {                                                                                    \
        field_role, (uint16_t) offsetof(type, member),                                    \
            (uint16_t) sizeof(((type *) 0)->member), (uint16_t) _Alignof(member_type),    \
            field_encoding                                                               \
    }

static bool make_dynamic_value(XrRuntimeDynamicValueAbi *out,
                               uint8_t target_endian) {
    static const XrCanonicalEncoding tags[] = {
        {"xray.runtime.dynamic-tag.v1/null", XR_TAG_NULL},
        {"xray.runtime.dynamic-tag.v1/bool", XR_TAG_BOOL},
        {"xray.runtime.dynamic-tag.v1/rune", XR_TAG_RUNE},
        {"xray.runtime.dynamic-tag.v1/i64", XR_TAG_I64},
        {"xray.runtime.dynamic-tag.v1/f64", XR_TAG_F64},
        {"xray.runtime.dynamic-tag.v1/object-reference", XR_TAG_PTR},
        {"xray.runtime.dynamic-tag.v1/aggregate-reference", XR_TAG_AGG_REF},
        {"xray.runtime.dynamic-tag.v1/not-found", XR_TAG_NOTFOUND},
        {"xray.runtime.dynamic-tag.v1/place", XR_TAG_PLACE},
    };
    *out = (XrRuntimeDynamicValueAbi) {
        .schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .size = (uint16_t) sizeof(XrValue),
        .alignment = (uint16_t) _Alignof(XrValue),
        .target_endian = target_endian,
        .padding_policy = XR_RUNTIME_PADDING_MUST_BE_ZERO,
        .tag_encoding_width = (uint8_t) sizeof(((XrValue *) 0)->tag),
        .flags_encoding_width = (uint8_t) sizeof(((XrValue *) 0)->flags),
        .object_reference_width = (uint8_t) sizeof(void *),
        .invalid_tag = UINT8_MAX,
        .null_tag = XR_TAG_NULL,
        .object_reference_tag = XR_TAG_PTR,
        .valid_flags_mask = XR_VALUE_FLAG_HEADER_AT_PTR,
        .reserved_zero_mask = UINT8_MAX & ~XR_VALUE_FLAG_HEADER_AT_PTR,
        .tag_count = (uint16_t) (sizeof(tags) / sizeof(tags[0])),
    };
    const XrCanonicalField fields[] = {
        {XR_RUNTIME_FIELD_DYN_TAG, (uint16_t) offsetof(XrValue, tag),
         (uint16_t) sizeof(((XrValue *) 0)->tag),
         (uint16_t) _Alignof(uint8_t), XR_RUNTIME_FIELD_UNSIGNED},
        {XR_RUNTIME_FIELD_DYN_FLAGS, (uint16_t) offsetof(XrValue, flags),
         (uint16_t) sizeof(((XrValue *) 0)->flags),
         (uint16_t) _Alignof(uint8_t), XR_RUNTIME_FIELD_BITSET},
        {XR_RUNTIME_FIELD_DYN_PAYLOAD, (uint16_t) offsetof(XrValue, i),
         (uint16_t) sizeof(((XrValue *) 0)->i),
         (uint16_t) _Alignof(int64_t), XR_RUNTIME_FIELD_OPAQUE_BITS},
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        set_field(&out->fields[i], &fields[i]);
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        XrRuntimeDynamicTagAbiEntry *tag = &out->tags[i];
        if (!canonical_id(tags[i].key, &tag->stable_id))
            return false;
        tag->encoding = tags[i].encoding;
        tag->payload_kind = tags[i].encoding == XR_TAG_NULL
                                ? XR_RUNTIME_DYN_PAYLOAD_NONE
                            : tags[i].encoding == XR_TAG_PTR
                                ? XR_RUNTIME_DYN_PAYLOAD_OBJECT_REFERENCE
                                : XR_RUNTIME_DYN_PAYLOAD_INLINE_BITS;
        if (tags[i].encoding == XR_TAG_PTR)
            tag->allowed_flags = XR_VALUE_FLAG_HEADER_AT_PTR;
    }
    qsort(out->tags, out->tag_count, sizeof(out->tags[0]),
          compare_stable_id_first);
    return true;
}

static bool make_domain_record(XrRuntimeRecordAbi *out) {
    static const XrCanonicalField fields[] = {
        XR_AUTHORITY_FIELD(XrRuntimeDomainIdentity, contract_id, XrStableId,
                           XR_RUNTIME_FIELD_DOMAIN_CONTRACT_ID,
                           XR_RUNTIME_FIELD_STABLE_ID),
        XR_AUTHORITY_FIELD(XrRuntimeDomainIdentity, instance_id, uint32_t,
                           XR_RUNTIME_FIELD_DOMAIN_INSTANCE_ID,
                           XR_RUNTIME_FIELD_UNSIGNED),
        XR_AUTHORITY_FIELD(XrRuntimeDomainIdentity, semantic_domain, uint8_t,
                           XR_RUNTIME_FIELD_DOMAIN_SEMANTIC,
                           XR_RUNTIME_FIELD_UNSIGNED),
        XR_AUTHORITY_FIELD(XrRuntimeDomainIdentity, materialization, uint8_t,
                           XR_RUNTIME_FIELD_DOMAIN_MATERIALIZATION,
                           XR_RUNTIME_FIELD_UNSIGNED),
    };
    static const XrCanonicalEncoding domains[] = {
        {"xray.runtime.storage-domain.v1/exec-local", XR_STORAGE_EXEC_LOCAL},
        {"xray.runtime.storage-domain.v1/transferable", XR_STORAGE_TRANSFERABLE},
        {"xray.runtime.storage-domain.v1/const-shared", XR_STORAGE_CONST_SHARED},
        {"xray.runtime.storage-domain.v1/sync-shared", XR_STORAGE_SYNC_SHARED},
        {"xray.runtime.storage-domain.v1/module-static", XR_STORAGE_MODULE_STATIC},
        {"xray.runtime.storage-domain.v1/foreign", XR_STORAGE_FOREIGN},
    };
    static const XrCanonicalEncoding materializations[] = {
        {"xray.runtime.materialization.v1/inline", XR_MATERIALIZE_INLINE},
        {"xray.runtime.materialization.v1/stack", XR_MATERIALIZE_STACK},
        {"xray.runtime.materialization.v1/static-data", XR_MATERIALIZE_STATIC_DATA},
        {"xray.runtime.materialization.v1/exec-heap", XR_MATERIALIZE_EXEC_HEAP},
        {"xray.runtime.materialization.v1/system-heap", XR_MATERIALIZE_SYSTEM_HEAP},
        {"xray.runtime.materialization.v1/sroa", XR_MATERIALIZE_SROA},
        {"xray.runtime.materialization.v1/external", XR_MATERIALIZE_EXTERNAL},
    };
    make_record(out, XR_RUNTIME_RECORD_DOMAIN_IDENTITY,
                (uint16_t) sizeof(XrRuntimeDomainIdentity),
                (uint16_t) _Alignof(XrRuntimeDomainIdentity), fields,
                sizeof(fields) / sizeof(fields[0]));
    out->namespace_count = 2;
    return make_namespace(&out->namespaces[0],
                          XR_RUNTIME_NAMESPACE_SEMANTIC_DOMAIN,
                          XR_RUNTIME_NAMESPACE_ENUM, 1,
                          XR_STORAGE_DOMAIN_UNKNOWN, domains,
                          sizeof(domains) / sizeof(domains[0])) &&
           make_namespace(&out->namespaces[1],
                          XR_RUNTIME_NAMESPACE_MATERIALIZATION,
                          XR_RUNTIME_NAMESPACE_ENUM, 1,
                          XR_MATERIALIZE_INVALID, materializations,
                          sizeof(materializations) / sizeof(materializations[0]));
}

static bool make_extent_record(XrRuntimeRecordAbi *out) {
    static const XrCanonicalField fields[] = {
        XR_AUTHORITY_FIELD(XrRuntimeExtentDescriptor, schema_version, uint32_t,
                           XR_RUNTIME_FIELD_EXTENT_SCHEMA, XR_RUNTIME_FIELD_UNSIGNED),
        XR_AUTHORITY_FIELD(XrRuntimeExtentDescriptor, id, XrStableId,
                           XR_RUNTIME_FIELD_EXTENT_ID, XR_RUNTIME_FIELD_STABLE_ID),
        XR_AUTHORITY_FIELD(XrRuntimeExtentDescriptor, layout_id, XrStableId,
                           XR_RUNTIME_FIELD_EXTENT_LAYOUT_ID, XR_RUNTIME_FIELD_STABLE_ID),
        XR_AUTHORITY_FIELD(XrRuntimeExtentDescriptor, group_id, XrStableId,
                           XR_RUNTIME_FIELD_EXTENT_GROUP_ID, XR_RUNTIME_FIELD_STABLE_ID),
        XR_AUTHORITY_FIELD(XrRuntimeExtentDescriptor, provider_id, XrStableId,
                           XR_RUNTIME_FIELD_EXTENT_PROVIDER_ID, XR_RUNTIME_FIELD_STABLE_ID),
        XR_AUTHORITY_FIELD(XrRuntimeExtentDescriptor, tail_offset, uint64_t,
                           XR_RUNTIME_FIELD_EXTENT_TAIL_OFFSET, XR_RUNTIME_FIELD_UNSIGNED),
        XR_AUTHORITY_FIELD(XrRuntimeExtentDescriptor, stride, uint64_t,
                           XR_RUNTIME_FIELD_EXTENT_STRIDE, XR_RUNTIME_FIELD_UNSIGNED),
        XR_AUTHORITY_FIELD(XrRuntimeExtentDescriptor, operand_index, uint16_t,
                           XR_RUNTIME_FIELD_EXTENT_OPERAND_INDEX, XR_RUNTIME_FIELD_UNSIGNED),
        XR_AUTHORITY_FIELD(XrRuntimeExtentDescriptor, part_index, uint16_t,
                           XR_RUNTIME_FIELD_EXTENT_PART_INDEX, XR_RUNTIME_FIELD_UNSIGNED),
        XR_AUTHORITY_FIELD(XrRuntimeExtentDescriptor, part_count, uint16_t,
                           XR_RUNTIME_FIELD_EXTENT_PART_COUNT, XR_RUNTIME_FIELD_UNSIGNED),
        XR_AUTHORITY_FIELD(XrRuntimeExtentDescriptor, kind, uint8_t,
                           XR_RUNTIME_FIELD_EXTENT_KIND, XR_RUNTIME_FIELD_UNSIGNED),
        XR_AUTHORITY_FIELD(XrRuntimeExtentDescriptor, fingerprint, XrFingerprint,
                           XR_RUNTIME_FIELD_EXTENT_FINGERPRINT,
                           XR_RUNTIME_FIELD_FINGERPRINT),
    };
    static const XrCanonicalEncoding kinds[] = {
        {"xray.runtime.extent-kind.v1/fixed", XR_RUNTIME_EXTENT_FIXED},
        {"xray.runtime.extent-kind.v1/inline-tail", XR_RUNTIME_EXTENT_INLINE_TAIL},
        {"xray.runtime.extent-kind.v1/external-buffer", XR_RUNTIME_EXTENT_EXTERNAL_BUFFER},
        {"xray.runtime.extent-kind.v1/multi-buffer", XR_RUNTIME_EXTENT_MULTI_BUFFER},
        {"xray.runtime.extent-kind.v1/provider-defined", XR_RUNTIME_EXTENT_PROVIDER_DEFINED},
    };
    make_record(out, XR_RUNTIME_RECORD_EXTENT_DESCRIPTOR,
                (uint16_t) sizeof(XrRuntimeExtentDescriptor),
                (uint16_t) _Alignof(XrRuntimeExtentDescriptor), fields,
                sizeof(fields) / sizeof(fields[0]));
    out->namespace_count = 2;
    if (!make_namespace(&out->namespaces[0], XR_RUNTIME_NAMESPACE_EXTENT_KIND,
                        XR_RUNTIME_NAMESPACE_ENUM, 1, UINT8_MAX, kinds,
                        sizeof(kinds) / sizeof(kinds[0])))
        return false;
    make_sentinel_namespace(&out->namespaces[1],
                            XR_RUNTIME_NAMESPACE_EXTENT_OPERAND, 2,
                            XR_RUNTIME_EXTENT_OPERAND_NONE);
    return true;
}

static bool make_layout_record(XrRuntimeRecordAbi *out) {
    static const XrCanonicalField fields[] = {
        XR_AUTHORITY_FIELD(XrRuntimeLayoutDescriptor, schema_version, uint32_t,
                           XR_RUNTIME_FIELD_LAYOUT_SCHEMA, XR_RUNTIME_FIELD_UNSIGNED),
        XR_AUTHORITY_FIELD(XrRuntimeLayoutDescriptor, descriptor_id, XrStableId,
                           XR_RUNTIME_FIELD_LAYOUT_DESCRIPTOR_ID, XR_RUNTIME_FIELD_STABLE_ID),
        XR_AUTHORITY_FIELD(XrRuntimeLayoutDescriptor, layout_id, XrStableId,
                           XR_RUNTIME_FIELD_LAYOUT_ID, XR_RUNTIME_FIELD_STABLE_ID),
        XR_AUTHORITY_FIELD(XrRuntimeLayoutDescriptor, object_kind_id, XrStableId,
                           XR_RUNTIME_FIELD_LAYOUT_OBJECT_KIND_ID, XR_RUNTIME_FIELD_STABLE_ID),
        XR_AUTHORITY_FIELD(XrRuntimeLayoutDescriptor, extent_id, XrStableId,
                           XR_RUNTIME_FIELD_LAYOUT_EXTENT_ID, XR_RUNTIME_FIELD_STABLE_ID),
        XR_AUTHORITY_FIELD(XrRuntimeLayoutDescriptor, root_plan_id, XrStableId,
                           XR_RUNTIME_FIELD_LAYOUT_ROOT_PLAN_ID, XR_RUNTIME_FIELD_STABLE_ID),
        XR_AUTHORITY_FIELD(XrRuntimeLayoutDescriptor, destructor_id, XrStableId,
                           XR_RUNTIME_FIELD_LAYOUT_DESTRUCTOR_ID, XR_RUNTIME_FIELD_STABLE_ID),
        XR_AUTHORITY_FIELD(XrRuntimeLayoutDescriptor, clone_id, XrStableId,
                           XR_RUNTIME_FIELD_LAYOUT_CLONE_ID, XR_RUNTIME_FIELD_STABLE_ID),
        XR_AUTHORITY_FIELD(XrRuntimeLayoutDescriptor, eq_hash_id, XrStableId,
                           XR_RUNTIME_FIELD_LAYOUT_EQ_HASH_ID, XR_RUNTIME_FIELD_STABLE_ID),
        XR_AUTHORITY_FIELD(XrRuntimeLayoutDescriptor, extent_fingerprint, XrFingerprint,
                           XR_RUNTIME_FIELD_LAYOUT_EXTENT_FINGERPRINT,
                           XR_RUNTIME_FIELD_FINGERPRINT),
        XR_AUTHORITY_FIELD(XrRuntimeLayoutDescriptor, fixed_prefix_size, uint64_t,
                           XR_RUNTIME_FIELD_LAYOUT_FIXED_PREFIX_SIZE,
                           XR_RUNTIME_FIELD_UNSIGNED),
        XR_AUTHORITY_FIELD(XrRuntimeLayoutDescriptor, alignment, uint32_t,
                           XR_RUNTIME_FIELD_LAYOUT_ALIGNMENT, XR_RUNTIME_FIELD_UNSIGNED),
        XR_AUTHORITY_FIELD(XrRuntimeLayoutDescriptor, allowed_semantic_domains, uint32_t,
                           XR_RUNTIME_FIELD_LAYOUT_SEMANTIC_DOMAINS,
                           XR_RUNTIME_FIELD_BITSET),
        XR_AUTHORITY_FIELD(XrRuntimeLayoutDescriptor, allowed_materializations, uint32_t,
                           XR_RUNTIME_FIELD_LAYOUT_MATERIALIZATIONS,
                           XR_RUNTIME_FIELD_BITSET),
        XR_AUTHORITY_FIELD(XrRuntimeLayoutDescriptor, flags, uint32_t,
                           XR_RUNTIME_FIELD_LAYOUT_FLAGS, XR_RUNTIME_FIELD_BITSET),
        XR_AUTHORITY_FIELD(XrRuntimeLayoutDescriptor, fingerprint, XrFingerprint,
                           XR_RUNTIME_FIELD_LAYOUT_FINGERPRINT,
                           XR_RUNTIME_FIELD_FINGERPRINT),
    };
    static const XrCanonicalEncoding flags[] = {
        {"xray.runtime.layout-flag.v1/has-roots", XR_LAYOUT_HAS_ROOTS},
        {"xray.runtime.layout-flag.v1/has-destructor", XR_LAYOUT_HAS_DESTRUCTOR},
        {"xray.runtime.layout-flag.v1/has-clone", XR_LAYOUT_HAS_CLONE},
        {"xray.runtime.layout-flag.v1/has-eq-hash", XR_LAYOUT_HAS_EQ_HASH},
    };
    make_record(out, XR_RUNTIME_RECORD_LAYOUT_DESCRIPTOR,
                (uint16_t) sizeof(XrRuntimeLayoutDescriptor),
                (uint16_t) _Alignof(XrRuntimeLayoutDescriptor), fields,
                sizeof(fields) / sizeof(fields[0]));
    out->namespace_count = 1;
    return make_namespace(&out->namespaces[0], XR_RUNTIME_NAMESPACE_LAYOUT_FLAGS,
                          XR_RUNTIME_NAMESPACE_BITMASK, 4, 0, flags,
                          sizeof(flags) / sizeof(flags[0]));
}

static void make_leaf_records(XrRuntimeAbiContract *abi) {
    static const XrCanonicalField limits[] = {
        XR_AUTHORITY_FIELD(XrRuntimeExtentLimits, max_allocation_bytes, uint64_t,
                           XR_RUNTIME_FIELD_LIMIT_MAX_ALLOCATION_BYTES,
                           XR_RUNTIME_FIELD_UNSIGNED),
        XR_AUTHORITY_FIELD(XrRuntimeExtentLimits, max_alignment, uint32_t,
                           XR_RUNTIME_FIELD_LIMIT_MAX_ALIGNMENT,
                           XR_RUNTIME_FIELD_UNSIGNED),
    };
    static const XrCanonicalField evaluated[] = {
        XR_AUTHORITY_FIELD(XrRuntimeEvaluatedExtent, extent_id, XrStableId,
                           XR_RUNTIME_FIELD_EVALUATED_EXTENT_ID,
                           XR_RUNTIME_FIELD_STABLE_ID),
        XR_AUTHORITY_FIELD(XrRuntimeEvaluatedExtent, extent_fingerprint, XrFingerprint,
                           XR_RUNTIME_FIELD_EVALUATED_EXTENT_FINGERPRINT,
                           XR_RUNTIME_FIELD_FINGERPRINT),
        XR_AUTHORITY_FIELD(XrRuntimeEvaluatedExtent, bytes, uint64_t,
                           XR_RUNTIME_FIELD_EVALUATED_BYTES,
                           XR_RUNTIME_FIELD_UNSIGNED),
        XR_AUTHORITY_FIELD(XrRuntimeEvaluatedExtent, operand, uint64_t,
                           XR_RUNTIME_FIELD_EVALUATED_OPERAND,
                           XR_RUNTIME_FIELD_UNSIGNED),
        XR_AUTHORITY_FIELD(XrRuntimeEvaluatedExtent, alignment, uint32_t,
                           XR_RUNTIME_FIELD_EVALUATED_ALIGNMENT,
                           XR_RUNTIME_FIELD_UNSIGNED),
        XR_AUTHORITY_FIELD(XrRuntimeEvaluatedExtent, part_index, uint16_t,
                           XR_RUNTIME_FIELD_EVALUATED_PART_INDEX,
                           XR_RUNTIME_FIELD_UNSIGNED),
        XR_AUTHORITY_FIELD(XrRuntimeEvaluatedExtent, part_count, uint16_t,
                           XR_RUNTIME_FIELD_EVALUATED_PART_COUNT,
                           XR_RUNTIME_FIELD_UNSIGNED),
    };
    static const XrCanonicalField group[] = {
        XR_AUTHORITY_FIELD(XrRuntimeExtentGroupSummary, group_id, XrStableId,
                           XR_RUNTIME_FIELD_GROUP_ID, XR_RUNTIME_FIELD_STABLE_ID),
        XR_AUTHORITY_FIELD(XrRuntimeExtentGroupSummary, part_count, uint16_t,
                           XR_RUNTIME_FIELD_GROUP_PART_COUNT,
                           XR_RUNTIME_FIELD_UNSIGNED),
        XR_AUTHORITY_FIELD(XrRuntimeExtentGroupSummary, fingerprint, XrFingerprint,
                           XR_RUNTIME_FIELD_GROUP_FINGERPRINT,
                           XR_RUNTIME_FIELD_FINGERPRINT),
    };
    make_record(&abi->extent_limits, XR_RUNTIME_RECORD_EXTENT_LIMITS,
                (uint16_t) sizeof(XrRuntimeExtentLimits),
                (uint16_t) _Alignof(XrRuntimeExtentLimits), limits,
                sizeof(limits) / sizeof(limits[0]));
    make_record(&abi->evaluated_extent, XR_RUNTIME_RECORD_EVALUATED_EXTENT,
                (uint16_t) sizeof(XrRuntimeEvaluatedExtent),
                (uint16_t) _Alignof(XrRuntimeEvaluatedExtent), evaluated,
                sizeof(evaluated) / sizeof(evaluated[0]));
    make_record(&abi->extent_group_summary,
                XR_RUNTIME_RECORD_EXTENT_GROUP_SUMMARY,
                (uint16_t) sizeof(XrRuntimeExtentGroupSummary),
                (uint16_t) _Alignof(XrRuntimeExtentGroupSummary), group,
                sizeof(group) / sizeof(group[0]));
}

static bool make_callback(XrRuntimeExtentProviderCallbackAbi *out) {
    static const char *const status_names[] = {
        "ok", "invalid-argument", "invalid-schema", "invalid-identity",
        "invalid-kind", "invalid-alignment", "invalid-domain", "invalid-extent",
        "fingerprint-mismatch", "overflow", "limit-exceeded", "provider-required",
        "provider-rejected", "invalid-group", "invalid-shape", "invalid-order",
        "invalid-overlap", "invalid-mask", "invalid-policy", "invalid-provider-set",
        "budget-exceeded",
    };
    _Static_assert(sizeof(status_names) / sizeof(status_names[0]) ==
                       XR_RUNTIME_ABI_STATUS_COUNT,
                   "runtime status authority is incomplete");
    XrCanonicalEncoding entries[XR_RUNTIME_ABI_STATUS_COUNT];
    char keys[XR_RUNTIME_ABI_STATUS_COUNT][80];
    for (size_t i = 0; i < XR_RUNTIME_ABI_STATUS_COUNT; i++) {
        int written = snprintf(keys[i], sizeof(keys[i]),
                               "xray.runtime.status.v1/%s", status_names[i]);
        if (written < 0 || (size_t) written >= sizeof(keys[i]))
            return false;
        entries[i].key = keys[i];
        entries[i].encoding = i;
    }
    *out = (XrRuntimeExtentProviderCallbackAbi) {
        .schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .provider_id_width = XR_STABLE_ID_BYTES,
        .operand_element_width = sizeof(uint64_t),
        .operand_count_width = sizeof(size_t),
        .result_width = sizeof(uint64_t),
        .error_normalization = XR_RUNTIME_PROVIDER_ERROR_NON_OK_TO_REJECTED,
    };
    return canonical_id("xray.runtime.extent-provider-callback.v1",
                        &out->contract_id) &&
           make_namespace(&out->status_namespace, XR_RUNTIME_NAMESPACE_STATUS,
                          XR_RUNTIME_NAMESPACE_ENUM, 1, UINT8_MAX, entries,
                          XR_RUNTIME_ABI_STATUS_COUNT);
}

static XrTargetProviderCallSlotAbi make_call_slot(uint8_t kind, uint8_t width,
                                                  uint8_t alignment,
                                                  uint8_t ownership,
                                                  uint8_t flags) {
    return (XrTargetProviderCallSlotAbi) {
        .value_kind = kind,
        .width = width,
        .alignment = alignment,
        .ownership = ownership,
        .flags = flags,
    };
}

static XrTargetProviderCallAbiContract make_call_abi(
    XrTargetProviderCallSlotAbi result,
    const XrTargetProviderCallSlotAbi *parameters, uint16_t count,
    uint8_t target_endian) {
    XrTargetProviderCallAbiContract abi = {
        .schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .parameter_count = count,
        .calling_convention = XR_TARGET_PROVIDER_CALLING_CONVENTION_C,
        .target_endian = target_endian,
        .pointer_width = (uint8_t) sizeof(void *),
        .pointer_alignment = (uint8_t) _Alignof(void *),
        .result = result,
    };
    for (size_t i = 0; i < count; i++)
        abi.parameters[i] = parameters[i];
    return abi;
}

static bool make_operation(XrTargetProviderOperationContract *out,
                           const char *key,
                           XrTargetProviderCallAbiContract call_abi,
                           uint32_t effects, uint32_t lifetime,
                           uint32_t failures) {
    *out = (XrTargetProviderOperationContract) {
        .call_abi = call_abi,
        .effect_flags = effects,
        .lifetime_flags = lifetime,
        .failure_flags = failures,
    };
    return canonical_id(key, &out->stable_id);
}

static bool make_hosted_providers(XrTargetProviderContract providers[2],
                                  uint8_t target_endian) {
    uint8_t pointer_width = (uint8_t) sizeof(void *);
    uint8_t pointer_alignment = (uint8_t) _Alignof(void *);
    XrTargetProviderCallSlotAbi void_result = make_call_slot(
        XR_TARGET_PROVIDER_CALL_VALUE_VOID, 0, 0,
        XR_TARGET_PROVIDER_CALL_OWNERSHIP_NONE, 0);
    XrTargetProviderCallSlotAbi usize_slot = make_call_slot(
        XR_TARGET_PROVIDER_CALL_VALUE_UNSIGNED_INTEGER, pointer_width,
        pointer_alignment, XR_TARGET_PROVIDER_CALL_OWNERSHIP_NONE, 0);
    XrTargetProviderCallSlotAbi allocate_result = make_call_slot(
        XR_TARGET_PROVIDER_CALL_VALUE_DATA_ADDRESS, pointer_width,
        pointer_alignment, XR_TARGET_PROVIDER_CALL_OWNERSHIP_RETURNED_OWNED,
        XR_TARGET_PROVIDER_CALL_SLOT_NULLABLE);
    XrTargetProviderCallSlotAbi allocate_parameters[] = {usize_slot, usize_slot};
    XrTargetProviderCallSlotAbi free_parameters[] = {
        make_call_slot(XR_TARGET_PROVIDER_CALL_VALUE_DATA_ADDRESS, pointer_width,
                       pointer_alignment,
                       XR_TARGET_PROVIDER_CALL_OWNERSHIP_CONSUMED,
                       XR_TARGET_PROVIDER_CALL_SLOT_NULLABLE),
        usize_slot,
        usize_slot,
    };
    XrTargetProviderCallSlotAbi panic_parameters[] = {
        make_call_slot(XR_TARGET_PROVIDER_CALL_VALUE_DATA_ADDRESS, pointer_width,
                       pointer_alignment,
                       XR_TARGET_PROVIDER_CALL_OWNERSHIP_BORROWED,
                       XR_TARGET_PROVIDER_CALL_SLOT_CONST_POINTEE),
        usize_slot,
    };
    memset(providers, 0, 2 * sizeof(providers[0]));
    providers[0] = (XrTargetProviderContract) {
        .schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .abi_schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .flags = XR_TARGET_PROVIDER_AVAILABLE_HOSTED,
        .operation_count = 2,
        .runtime_profile = XR_TARGET_RUNTIME_PROFILE_HOSTED,
        .provider_kind = XR_TARGET_PROVIDER_ALLOCATOR,
        .allocator_max_alignment = (uint32_t) _Alignof(void *),
        .allocator_sized_free = 0,
        .allocator_zeroed_allocation = 0,
        .allocator_thread_safe = 1,
    };
    if (!canonical_id("xray.runtime.provider.v1/hosted/allocator",
                      &providers[0].contract_id) ||
        !make_operation(
            &providers[0].operations[0],
            "xray.runtime.provider-operation.v1/hosted/allocator/allocate",
            make_call_abi(allocate_result, allocate_parameters, 2,
                          target_endian),
            XR_TARGET_PROVIDER_EFFECT_ALLOCATES,
            XR_TARGET_PROVIDER_LIFETIME_RETURNS_OWNED,
            0) ||
        !make_operation(
            &providers[0].operations[1],
            "xray.runtime.provider-operation.v1/hosted/allocator/deallocate",
            make_call_abi(void_result, free_parameters, 3, target_endian),
            XR_TARGET_PROVIDER_EFFECT_DEALLOCATES,
            XR_TARGET_PROVIDER_LIFETIME_CONSUMES_OWNED, 0))
        return false;
    qsort(providers[0].operations, providers[0].operation_count,
          sizeof(providers[0].operations[0]), compare_stable_id_first);

    providers[1] = (XrTargetProviderContract) {
        .schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .abi_schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .flags = XR_TARGET_PROVIDER_AVAILABLE_HOSTED,
        .operation_count = 1,
        .runtime_profile = XR_TARGET_RUNTIME_PROFILE_HOSTED,
        .provider_kind = XR_TARGET_PROVIDER_PANIC,
        .panic_behavior = XR_TARGET_PROVIDER_PANIC_NO_RETURN,
    };
    return canonical_id("xray.runtime.provider.v1/hosted/panic",
                        &providers[1].contract_id) &&
           make_operation(
               &providers[1].operations[0],
               "xray.runtime.provider-operation.v1/hosted/panic/raise",
               make_call_abi(void_result, panic_parameters, 2, target_endian),
               XR_TARGET_PROVIDER_EFFECT_PANICS,
               XR_TARGET_PROVIDER_LIFETIME_BORROWS,
               XR_TARGET_PROVIDER_FAILURE_NO_RETURN);
}

XrRuntimeAbiStatus xr_runtime_target_authority_native_hosted(
    XrRuntimeTargetAuthority *out) {
    if (!out)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    XrRuntimeTargetAuthority authority;
    memset(&authority, 0, sizeof(authority));
    XrRuntimeAbiStatus status =
        xr_runtime_object_header_native_materialization_facts(
            &authority.object_header_materialization);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    uint8_t target_endian = authority.object_header_materialization.target_endian;
    XrRuntimeAbiContract *abi = &authority.runtime_abi;
    *abi = (XrRuntimeAbiContract) {
        .schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .stable_id_width = XR_STABLE_ID_BYTES,
        .fingerprint_width = XR_FINGERPRINT_BYTES,
        .pointer_width = sizeof(void *),
        .canonical_serialization_endian = XR_RUNTIME_ENDIAN_LITTLE,
        .target_endian = target_endian,
        .checked_arithmetic_policy = XR_RUNTIME_CHECKED_ARITHMETIC_REJECT_OVERFLOW,
        .alignment_policy = XR_RUNTIME_ALIGNMENT_POWER_OF_TWO_REJECT_OVERFLOW,
        .unknown_enum_policy = XR_RUNTIME_UNKNOWN_ENUM_REJECT,
        .reserved_zero_policy = XR_RUNTIME_RESERVED_ZERO_REJECT,
    };
    status = xr_runtime_object_header_abi_materialize(
        &authority.object_header_materialization, &abi->object_header);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    if (!make_dynamic_value(&abi->dynamic_value, target_endian) ||
        !make_domain_record(&abi->domain_identity) ||
        !make_extent_record(&abi->extent_descriptor) ||
        !make_layout_record(&abi->layout_descriptor) || !make_callback(
            &abi->extent_provider_callback) ||
        !make_hosted_providers(authority.providers, target_endian))
        return XR_RUNTIME_ABI_INVALID_IDENTITY;
    make_leaf_records(abi);
    XrFingerprint fingerprint;
    status = xr_runtime_abi_contract_fingerprint(abi, &fingerprint);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    uint64_t provider_mask = 0;
    status = xr_target_provider_set_fingerprint(
        authority.providers, XR_RUNTIME_TARGET_AUTHORITY_PROVIDER_COUNT,
        &provider_mask, &fingerprint);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    authority.provider_count = XR_RUNTIME_TARGET_AUTHORITY_PROVIDER_COUNT;
    *out = authority;
    return XR_RUNTIME_ABI_OK;
}
