/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_runtime_string_object.c - Canonical string object contract
 */

#include "xr_runtime_string_object.h"
#include "../../base/xsha256.h"
#include <string.h>

static const uint8_t string_contract_domain[] =
    "xray-runtime-string-contract-v2\0";
static const uint8_t string_literal_contract_domain[] =
    "xray-runtime-string-literal-materialization-v1\0";

/* Stable ID canonical key: runtime.extent.string.inline-utf8.v1 */
static const XrStableId string_extent_id = {{
    0x1f, 0x20, 0x5a, 0xaa, 0xad, 0xfe, 0xd4, 0xe9,
    0x36, 0x9d, 0x1c, 0x78, 0x7f, 0xdc, 0x1a, 0x0b,
}};
/* Stable ID canonical key: runtime.layout.string.inline-utf8.v1 */
static const XrStableId string_layout_id = {{
    0xa6, 0x09, 0xee, 0xa8, 0x6e, 0x02, 0x40, 0x5f,
    0xdf, 0x5f, 0xad, 0x7c, 0xee, 0x60, 0x33, 0x40,
}};
/* Stable ID canonical key: runtime.layout-descriptor.string.inline-utf8.v1 */
static const XrStableId string_layout_descriptor_id = {{
    0xb5, 0x0a, 0x55, 0xed, 0x61, 0xdf, 0x14, 0xd3,
    0xef, 0x0a, 0xbc, 0x67, 0xb1, 0x55, 0x5e, 0x85,
}};
/* Stable ID canonical key: runtime.eq-hash.string.utf8-bytes.v1 */
static const XrStableId string_eq_hash_id = {{
    0xe0, 0x03, 0x4f, 0xb9, 0xfd, 0x83, 0x46, 0x99,
    0x05, 0xb1, 0x0e, 0x51, 0xce, 0xfe, 0xe5, 0x37,
}};
/* Stable ID canonical key: runtime.clone.string.inline-utf8.v1 */
static const XrStableId string_clone_id = {{
    0x0c, 0xa4, 0x53, 0x29, 0x8e, 0xf1, 0x05, 0x14,
    0x82, 0x3a, 0x55, 0x72, 0xe0, 0x07, 0x96, 0x59,
}};
/* Stable ID canonical keys, in runtime contract order:
 *   runtime.domain.string.exec-local.v1
 *   runtime.domain.string.transferable.v1
 *   runtime.domain.string.const-shared.v1
 *   runtime.domain.string.sync-shared.v1 */
static const XrStableId string_domain_ids[XR_RUNTIME_STRING_DOMAIN_COUNT] = {
    {{0x0a, 0x3a, 0x9f, 0x6b, 0xaa, 0xbf, 0xad, 0x03,
      0x84, 0xc3, 0x3c, 0x3c, 0x54, 0x34, 0x58, 0xae}},
    {{0x59, 0xfe, 0xbb, 0xa9, 0x1a, 0xeb, 0xf3, 0x61,
      0x67, 0x2a, 0x6a, 0xe0, 0x82, 0x37, 0x18, 0x4f}},
    {{0x71, 0xe4, 0xbd, 0xd5, 0xa8, 0x72, 0x5d, 0xa3,
      0xe3, 0xae, 0xc3, 0x20, 0xbb, 0x18, 0xeb, 0x67}},
    {{0x67, 0x3b, 0x89, 0x49, 0xce, 0x9a, 0x48, 0x49,
      0x10, 0xfa, 0x06, 0x2d, 0xf1, 0x4a, 0x82, 0x79}},
};

static bool bytes_equal(const void *left, const void *right, size_t size) {
    return memcmp(left, right, size) == 0;
}

static void hash_u16(XrSHA256Context *ctx, uint16_t value) {
    uint8_t bytes[2] = {(uint8_t) value, (uint8_t) (value >> 8)};
    xr_sha256_update(ctx, bytes, sizeof(bytes));
}

static void hash_u32(XrSHA256Context *ctx, uint32_t value) {
    uint8_t bytes[4];
    for (unsigned i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8));
    xr_sha256_update(ctx, bytes, sizeof(bytes));
}

static void hash_id(XrSHA256Context *ctx, XrStableId id) {
    xr_sha256_update(ctx, id.bytes, sizeof(id.bytes));
}

static bool reserved_zero(const XrRuntimeStringObjectContract *contract) {
    for (size_t i = 0; i < sizeof(contract->reserved) / sizeof(contract->reserved[0]); i++) {
        if (contract->reserved[i] != 0)
            return false;
    }
    return true;
}

static bool literal_reserved_zero(
    const XrRuntimeStringLiteralMaterializationContract *contract) {
    for (size_t i = 0;
         i < sizeof(contract->reserved) / sizeof(contract->reserved[0]); i++) {
        if (contract->reserved[i] != 0)
            return false;
    }
    return true;
}

static void build_literal_fields(XrRuntimeStringFieldDescriptor *fields) {
    fields[0] = (XrRuntimeStringFieldDescriptor) {
        .role = XR_RUNTIME_STRING_LITERAL_FIELD_BYTE_LENGTH,
        .offset = (uint32_t) offsetof(XrRuntimeStringLiteralView, len),
        .width = (uint32_t) sizeof(((XrRuntimeStringLiteralView *) 0)->len),
    };
    fields[1] = (XrRuntimeStringFieldDescriptor) {
        .role = XR_RUNTIME_STRING_LITERAL_FIELD_RUNE_LENGTH,
        .offset = (uint32_t) offsetof(XrRuntimeStringLiteralView, rune_len),
        .width = (uint32_t) sizeof(((XrRuntimeStringLiteralView *) 0)->rune_len),
    };
    fields[2] = (XrRuntimeStringFieldDescriptor) {
        .role = XR_RUNTIME_STRING_LITERAL_FIELD_HASH,
        .offset = (uint32_t) offsetof(XrRuntimeStringLiteralView, hash),
        .width = (uint32_t) sizeof(((XrRuntimeStringLiteralView *) 0)->hash),
    };
    fields[3] = (XrRuntimeStringFieldDescriptor) {
        .role = XR_RUNTIME_STRING_LITERAL_FIELD_FLAGS,
        .offset = (uint32_t) offsetof(XrRuntimeStringLiteralView, flags),
        .width = (uint32_t) sizeof(((XrRuntimeStringLiteralView *) 0)->flags),
    };
    fields[4] = (XrRuntimeStringFieldDescriptor) {
        .role = XR_RUNTIME_STRING_LITERAL_FIELD_UTF8_POINTER,
        .offset = (uint32_t) offsetof(XrRuntimeStringLiteralView, data),
        .width = (uint32_t) sizeof(((XrRuntimeStringLiteralView *) 0)->data),
    };
}

static XrRuntimeAbiStatus verify_literal_shape(
    const XrRuntimeStringLiteralMaterializationContract *contract) {
    static const uint16_t roles[XR_RUNTIME_STRING_LITERAL_FIELD_COUNT] = {
        XR_RUNTIME_STRING_LITERAL_FIELD_BYTE_LENGTH,
        XR_RUNTIME_STRING_LITERAL_FIELD_RUNE_LENGTH,
        XR_RUNTIME_STRING_LITERAL_FIELD_HASH,
        XR_RUNTIME_STRING_LITERAL_FIELD_FLAGS,
        XR_RUNTIME_STRING_LITERAL_FIELD_UTF8_POINTER,
    };
    if (!contract)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    if (contract->schema_version !=
        XR_RUNTIME_STRING_LITERAL_CONTRACT_SCHEMA_VERSION)
        return XR_RUNTIME_ABI_INVALID_SCHEMA;
    if (contract->dynamic_tag != XR_RUNTIME_STRING_LITERAL_DYNAMIC_TAG ||
        contract->has_object_header != 0 || contract->owns_utf8_bytes != 0 ||
        contract->nul_terminated != 1 ||
        contract->literal_flag != XR_RUNTIME_STRING_LITERAL_FLAG ||
        contract->semantic_domain != XR_STORAGE_CONST_SHARED ||
        contract->backend_materialization != XR_MATERIALIZE_STATIC_DATA ||
        contract->view_size != sizeof(XrRuntimeStringLiteralView) ||
        contract->view_alignment != _Alignof(XrRuntimeStringLiteralView) ||
        contract->field_count != XR_RUNTIME_STRING_LITERAL_FIELD_COUNT ||
        !literal_reserved_zero(contract))
        return XR_RUNTIME_ABI_INVALID_POLICY;
    XrRuntimeStringFieldDescriptor expected[XR_RUNTIME_STRING_LITERAL_FIELD_COUNT] = {0};
    build_literal_fields(expected);
    for (uint32_t i = 0; i < XR_RUNTIME_STRING_LITERAL_FIELD_COUNT; i++) {
        const XrRuntimeStringFieldDescriptor *field = &contract->fields[i];
        if (field->role != roles[i] || field->role != expected[i].role ||
            field->flags != 0 || field->offset != expected[i].offset ||
            field->width != expected[i].width || field->reserved != 0)
            return XR_RUNTIME_ABI_INVALID_SHAPE;
    }
    return XR_RUNTIME_ABI_OK;
}

XrRuntimeAbiStatus xr_runtime_string_literal_materialization_contract_fingerprint(
    const XrRuntimeStringLiteralMaterializationContract *contract,
    XrFingerprint *out) {
    if (!out)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    XrRuntimeAbiStatus status = verify_literal_shape(contract);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, string_literal_contract_domain,
                     sizeof(string_literal_contract_domain) - 1u);
    hash_u32(&ctx, contract->schema_version);
    xr_sha256_update(&ctx, &contract->dynamic_tag, 1);
    xr_sha256_update(&ctx, &contract->has_object_header, 1);
    xr_sha256_update(&ctx, &contract->owns_utf8_bytes, 1);
    xr_sha256_update(&ctx, &contract->nul_terminated, 1);
    hash_u32(&ctx, contract->literal_flag);
    hash_u32(&ctx, contract->semantic_domain);
    hash_u32(&ctx, contract->backend_materialization);
    hash_u32(&ctx, contract->view_size);
    hash_u16(&ctx, contract->view_alignment);
    hash_u16(&ctx, contract->field_count);
    for (uint32_t i = 0; i < XR_RUNTIME_STRING_LITERAL_FIELD_COUNT; i++) {
        hash_u16(&ctx, contract->fields[i].role);
        hash_u16(&ctx, contract->fields[i].flags);
        hash_u32(&ctx, contract->fields[i].offset);
        hash_u32(&ctx, contract->fields[i].width);
    }
    xr_sha256_final(&ctx, out->bytes);
    return XR_RUNTIME_ABI_OK;
}

XrRuntimeAbiStatus xr_runtime_string_literal_materialization_contract_build(
    XrRuntimeStringLiteralMaterializationContract *out) {
    if (!out)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    XrRuntimeStringLiteralMaterializationContract candidate = {
        .schema_version = XR_RUNTIME_STRING_LITERAL_CONTRACT_SCHEMA_VERSION,
        .dynamic_tag = XR_RUNTIME_STRING_LITERAL_DYNAMIC_TAG,
        .nul_terminated = 1,
        .literal_flag = XR_RUNTIME_STRING_LITERAL_FLAG,
        .semantic_domain = XR_STORAGE_CONST_SHARED,
        .backend_materialization = XR_MATERIALIZE_STATIC_DATA,
        .view_size = (uint32_t) sizeof(XrRuntimeStringLiteralView),
        .view_alignment = (uint16_t) _Alignof(XrRuntimeStringLiteralView),
        .field_count = XR_RUNTIME_STRING_LITERAL_FIELD_COUNT,
    };
    build_literal_fields(candidate.fields);
    XrRuntimeAbiStatus status =
        xr_runtime_string_literal_materialization_contract_fingerprint(
            &candidate, &candidate.fingerprint);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    *out = candidate;
    return XR_RUNTIME_ABI_OK;
}

XrRuntimeAbiStatus xr_runtime_string_literal_materialization_contract_verify(
    const XrRuntimeStringLiteralMaterializationContract *contract) {
    XrRuntimeAbiStatus status = verify_literal_shape(contract);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    XrFingerprint actual;
    status = xr_runtime_string_literal_materialization_contract_fingerprint(
        contract, &actual);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    return bytes_equal(actual.bytes, contract->fingerprint.bytes,
                       sizeof(actual.bytes))
               ? XR_RUNTIME_ABI_OK
               : XR_RUNTIME_ABI_FINGERPRINT_MISMATCH;
}

static void build_domains(XrRuntimeDomainIdentity *domains) {
    domains[XR_RUNTIME_STRING_DOMAIN_EXEC_LOCAL] = (XrRuntimeDomainIdentity) {
        .contract_id = string_domain_ids[XR_RUNTIME_STRING_DOMAIN_EXEC_LOCAL],
        .instance_id = 1,
        .semantic_domain = XR_STORAGE_EXEC_LOCAL,
        .materialization = XR_MATERIALIZE_EXEC_HEAP,
    };
    domains[XR_RUNTIME_STRING_DOMAIN_TRANSFERABLE] = (XrRuntimeDomainIdentity) {
        .contract_id = string_domain_ids[XR_RUNTIME_STRING_DOMAIN_TRANSFERABLE],
        .instance_id = 2,
        .semantic_domain = XR_STORAGE_TRANSFERABLE,
        .materialization = XR_MATERIALIZE_SYSTEM_HEAP,
    };
    domains[XR_RUNTIME_STRING_DOMAIN_CONST_SHARED] = (XrRuntimeDomainIdentity) {
        .contract_id = string_domain_ids[XR_RUNTIME_STRING_DOMAIN_CONST_SHARED],
        .instance_id = 3,
        .semantic_domain = XR_STORAGE_CONST_SHARED,
        .materialization = XR_MATERIALIZE_SYSTEM_HEAP,
    };
    domains[XR_RUNTIME_STRING_DOMAIN_SYNC_SHARED] = (XrRuntimeDomainIdentity) {
        .contract_id = string_domain_ids[XR_RUNTIME_STRING_DOMAIN_SYNC_SHARED],
        .instance_id = 4,
        .semantic_domain = XR_STORAGE_SYNC_SHARED,
        .materialization = XR_MATERIALIZE_SYSTEM_HEAP,
    };
}

static void build_fields(XrRuntimeStringFieldDescriptor *fields) {
    fields[0] = (XrRuntimeStringFieldDescriptor) {
        .role = XR_RUNTIME_STRING_FIELD_OBJECT_HEADER,
        .offset = 0,
        .width = XR_RUNTIME_OBJECT_HEADER_SIZE,
    };
    fields[1] = (XrRuntimeStringFieldDescriptor) {
        .role = XR_RUNTIME_STRING_FIELD_TRAITS,
        .flags = XR_RUNTIME_STRING_FIELD_ATOMIC,
        .offset = (uint32_t) offsetof(XrString, traits),
        .width = sizeof(uint16_t),
    };
    fields[2] = (XrRuntimeStringFieldDescriptor) {
        .role = XR_RUNTIME_STRING_FIELD_BYTE_LENGTH,
        .offset = (uint32_t) offsetof(XrString, length),
        .width = sizeof(uint32_t),
    };
    fields[3] = (XrRuntimeStringFieldDescriptor) {
        .role = XR_RUNTIME_STRING_FIELD_RUNE_LENGTH,
        .offset = (uint32_t) offsetof(XrString, rune_length),
        .width = sizeof(uint32_t),
    };
    fields[4] = (XrRuntimeStringFieldDescriptor) {
        .role = XR_RUNTIME_STRING_FIELD_HASH,
        .offset = (uint32_t) offsetof(XrString, hash),
        .width = sizeof(uint32_t),
    };
    fields[5] = (XrRuntimeStringFieldDescriptor) {
        .role = XR_RUNTIME_STRING_FIELD_UTF8_TAIL,
        .flags = XR_RUNTIME_STRING_FIELD_FLEXIBLE_TAIL,
        .offset = (uint32_t) offsetof(XrString, data),
        .width = sizeof(char),
    };
}

static void build_materializations(
    XrRuntimeStringMaterializationDescriptor *materializations) {
    materializations[0] = (XrRuntimeStringMaterializationDescriptor) {
        .kind = XR_RUNTIME_STRING_MATERIALIZATION_LITERAL_VIEW,
        .nul_terminated = 1,
        .semantic_domain_mask =
            XR_SEMANTIC_DOMAIN_MASK(XR_STORAGE_CONST_SHARED),
        .backend_materialization_mask =
            XR_MATERIALIZATION_MASK(XR_MATERIALIZE_STATIC_DATA),
    };
    materializations[1] = (XrRuntimeStringMaterializationDescriptor) {
        .kind = XR_RUNTIME_STRING_MATERIALIZATION_OWNED_OBJECT,
        .has_object_header = 1,
        .owns_utf8_bytes = 1,
        .nul_terminated = 1,
        .semantic_domain_mask =
            XR_SEMANTIC_DOMAIN_MASK(XR_STORAGE_EXEC_LOCAL) |
            XR_SEMANTIC_DOMAIN_MASK(XR_STORAGE_TRANSFERABLE) |
            XR_SEMANTIC_DOMAIN_MASK(XR_STORAGE_CONST_SHARED) |
            XR_SEMANTIC_DOMAIN_MASK(XR_STORAGE_SYNC_SHARED),
        .backend_materialization_mask =
            XR_MATERIALIZATION_MASK(XR_MATERIALIZE_EXEC_HEAP) |
            XR_MATERIALIZATION_MASK(XR_MATERIALIZE_SYSTEM_HEAP),
    };
}

static XrRuntimeAbiStatus build_unfingerprinted(
    XrRuntimeStringObjectContract *contract) {
    memset(contract, 0, sizeof(*contract));
    contract->schema_version = XR_RUNTIME_STRING_CONTRACT_SCHEMA_VERSION;
    contract->layout_index = XR_RUNTIME_STRING_LAYOUT_INDEX;
    contract->domain_count = XR_RUNTIME_STRING_DOMAIN_COUNT;
    contract->data_offset = XR_RUNTIME_STRING_FIXED_PREFIX_SIZE;
    contract->extent_operand_bias = 1;
    contract->maximum_byte_length =
        XR_RUNTIME_STRING_MAXIMUM_BYTE_LENGTH;
    contract->rune_length_unknown = UINT32_MAX;
    contract->object_kind = XR_RUNTIME_OBJECT_KIND_STRING;
    contract->trait_valid_mask = XR_RUNTIME_STRING_TRAIT_VALID_MASK;
    build_fields(contract->fields);
    build_materializations(contract->materializations);
    XrRuntimeAbiStatus status =
        xr_runtime_string_literal_materialization_contract_build(
            &contract->literal_view);
    if (status != XR_RUNTIME_ABI_OK)
        return status;

    contract->extent.schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION;
    contract->extent.id = string_extent_id;
    contract->extent.layout_id = string_layout_id;
    contract->extent.tail_offset = XR_RUNTIME_STRING_FIXED_PREFIX_SIZE;
    contract->extent.stride = 1;
    contract->extent.operand_index = 0;
    contract->extent.part_index = 0;
    contract->extent.part_count = 1;
    contract->extent.kind = XR_RUNTIME_EXTENT_INLINE_TAIL;
    status = xr_runtime_extent_descriptor_fingerprint(
        &contract->extent, &contract->extent.fingerprint);
    if (status != XR_RUNTIME_ABI_OK)
        return status;

    contract->layout.schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION;
    contract->layout.descriptor_id = string_layout_descriptor_id;
    contract->layout.layout_id = string_layout_id;
    status = xr_runtime_object_kind_stable_id(
        XR_RUNTIME_OBJECT_KIND_STRING, &contract->layout.object_kind_id);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    contract->layout.extent_id = string_extent_id;
    contract->layout.clone_id = string_clone_id;
    contract->layout.eq_hash_id = string_eq_hash_id;
    contract->layout.extent_fingerprint = contract->extent.fingerprint;
    contract->layout.fixed_prefix_size = XR_RUNTIME_STRING_FIXED_PREFIX_SIZE;
    contract->layout.alignment = XR_RUNTIME_OBJECT_HEADER_ALIGNMENT;
    contract->layout.allowed_semantic_domains =
        XR_SEMANTIC_DOMAIN_MASK(XR_STORAGE_EXEC_LOCAL) |
        XR_SEMANTIC_DOMAIN_MASK(XR_STORAGE_TRANSFERABLE) |
        XR_SEMANTIC_DOMAIN_MASK(XR_STORAGE_CONST_SHARED) |
        XR_SEMANTIC_DOMAIN_MASK(XR_STORAGE_SYNC_SHARED);
    contract->layout.allowed_materializations =
        XR_MATERIALIZATION_MASK(XR_MATERIALIZE_EXEC_HEAP) |
        XR_MATERIALIZATION_MASK(XR_MATERIALIZE_SYSTEM_HEAP);
    contract->layout.flags = XR_LAYOUT_HAS_CLONE | XR_LAYOUT_HAS_EQ_HASH;
    status = xr_runtime_layout_descriptor_fingerprint(
        &contract->layout, &contract->layout.fingerprint);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    build_domains(contract->domains);
    return XR_RUNTIME_ABI_OK;
}

static XrRuntimeAbiStatus verify_shape(
    const XrRuntimeStringObjectContract *contract) {
    if (!contract)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    if (contract->schema_version != XR_RUNTIME_STRING_CONTRACT_SCHEMA_VERSION)
        return XR_RUNTIME_ABI_INVALID_SCHEMA;
    if (contract->layout_index != XR_RUNTIME_STRING_LAYOUT_INDEX ||
        contract->domain_count != XR_RUNTIME_STRING_DOMAIN_COUNT ||
        contract->data_offset != XR_RUNTIME_STRING_FIXED_PREFIX_SIZE ||
        contract->extent_operand_bias != 1 ||
        contract->maximum_byte_length !=
            XR_RUNTIME_STRING_MAXIMUM_BYTE_LENGTH ||
        contract->rune_length_unknown != UINT32_MAX ||
        contract->object_kind != XR_RUNTIME_OBJECT_KIND_STRING)
        return XR_RUNTIME_ABI_INVALID_SHAPE;
    if (contract->trait_valid_mask != XR_RUNTIME_STRING_TRAIT_VALID_MASK)
        return XR_RUNTIME_ABI_INVALID_MASK;
    static const uint16_t expected_roles[XR_RUNTIME_STRING_FIELD_COUNT] = {
        XR_RUNTIME_STRING_FIELD_OBJECT_HEADER,
        XR_RUNTIME_STRING_FIELD_TRAITS,
        XR_RUNTIME_STRING_FIELD_BYTE_LENGTH,
        XR_RUNTIME_STRING_FIELD_RUNE_LENGTH,
        XR_RUNTIME_STRING_FIELD_HASH,
        XR_RUNTIME_STRING_FIELD_UTF8_TAIL,
    };
    for (uint32_t i = 0; i < XR_RUNTIME_STRING_FIELD_COUNT; i++) {
        if (contract->fields[i].role != expected_roles[i] ||
            contract->fields[i].reserved != 0 ||
            contract->fields[i].width == 0)
            return XR_RUNTIME_ABI_INVALID_SHAPE;
        if (i > 0 && contract->fields[i].offset <
                         contract->fields[i - 1].offset +
                             contract->fields[i - 1].width)
            return XR_RUNTIME_ABI_INVALID_OVERLAP;
    }
    if (contract->fields[0].offset != 0 ||
        contract->fields[0].width != XR_RUNTIME_OBJECT_HEADER_SIZE ||
        contract->fields[0].flags != 0 ||
        contract->fields[1].offset != offsetof(XrString, traits) ||
        contract->fields[1].width != sizeof(uint16_t) ||
        contract->fields[1].flags != XR_RUNTIME_STRING_FIELD_ATOMIC ||
        contract->fields[2].offset != offsetof(XrString, length) ||
        contract->fields[2].width != sizeof(uint32_t) ||
        contract->fields[2].flags != 0 ||
        contract->fields[3].offset != offsetof(XrString, rune_length) ||
        contract->fields[3].width != sizeof(uint32_t) ||
        contract->fields[3].flags != 0 ||
        contract->fields[4].offset != offsetof(XrString, hash) ||
        contract->fields[4].width != sizeof(uint32_t) ||
        contract->fields[4].flags != 0 ||
        contract->fields[5].offset != offsetof(XrString, data) ||
        contract->fields[5].width != sizeof(char) ||
        contract->fields[5].flags !=
            XR_RUNTIME_STRING_FIELD_FLEXIBLE_TAIL)
        return XR_RUNTIME_ABI_INVALID_SHAPE;
    const XrRuntimeStringMaterializationDescriptor *literal =
        &contract->materializations[0];
    const XrRuntimeStringMaterializationDescriptor *owned =
        &contract->materializations[1];
    if (literal->kind != XR_RUNTIME_STRING_MATERIALIZATION_LITERAL_VIEW ||
        literal->has_object_header != 0 || literal->owns_utf8_bytes != 0 ||
        literal->nul_terminated != 1 || literal->reserved != 0 ||
        literal->semantic_domain_mask !=
            XR_SEMANTIC_DOMAIN_MASK(XR_STORAGE_CONST_SHARED) ||
        literal->backend_materialization_mask !=
            XR_MATERIALIZATION_MASK(XR_MATERIALIZE_STATIC_DATA) ||
        owned->kind != XR_RUNTIME_STRING_MATERIALIZATION_OWNED_OBJECT ||
        owned->has_object_header != 1 || owned->owns_utf8_bytes != 1 ||
        owned->nul_terminated != 1 || owned->reserved != 0 ||
        owned->semantic_domain_mask !=
            (XR_SEMANTIC_DOMAIN_MASK(XR_STORAGE_EXEC_LOCAL) |
             XR_SEMANTIC_DOMAIN_MASK(XR_STORAGE_TRANSFERABLE) |
             XR_SEMANTIC_DOMAIN_MASK(XR_STORAGE_CONST_SHARED) |
             XR_SEMANTIC_DOMAIN_MASK(XR_STORAGE_SYNC_SHARED)) ||
        owned->backend_materialization_mask !=
            (XR_MATERIALIZATION_MASK(XR_MATERIALIZE_EXEC_HEAP) |
             XR_MATERIALIZATION_MASK(XR_MATERIALIZE_SYSTEM_HEAP)))
        return XR_RUNTIME_ABI_INVALID_POLICY;
    XrRuntimeAbiStatus literal_status =
        xr_runtime_string_literal_materialization_contract_verify(
            &contract->literal_view);
    if (literal_status != XR_RUNTIME_ABI_OK)
        return literal_status;
    if (!reserved_zero(contract))
        return XR_RUNTIME_ABI_INVALID_POLICY;
    XrRuntimeAbiStatus status = xr_runtime_layout_descriptor_verify(
        &contract->layout, &contract->extent);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    for (uint32_t i = 0; i < contract->domain_count; i++) {
        if (!xr_runtime_domain_identity_valid(contract->domains[i]) ||
            !xr_runtime_layout_allows_domain(&contract->layout,
                                             contract->domains[i]))
            return XR_RUNTIME_ABI_INVALID_DOMAIN;
    }
    return XR_RUNTIME_ABI_OK;
}

XrRuntimeAbiStatus xr_runtime_string_object_contract_fingerprint(
    const XrRuntimeStringObjectContract *contract, XrFingerprint *out) {
    if (!out)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    XrRuntimeAbiStatus status = verify_shape(contract);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, string_contract_domain,
                     sizeof(string_contract_domain) - 1);
    hash_u32(&ctx, contract->schema_version);
    hash_u32(&ctx, contract->layout_index);
    hash_u32(&ctx, contract->domain_count);
    hash_u32(&ctx, contract->data_offset);
    hash_u32(&ctx, contract->extent_operand_bias);
    hash_u32(&ctx, contract->maximum_byte_length);
    hash_u32(&ctx, contract->rune_length_unknown);
    hash_u16(&ctx, contract->object_kind);
    hash_u16(&ctx, contract->trait_valid_mask);
    for (uint32_t i = 0; i < XR_RUNTIME_STRING_FIELD_COUNT; i++) {
        hash_u32(&ctx, i);
        hash_u16(&ctx, contract->fields[i].role);
        hash_u16(&ctx, contract->fields[i].flags);
        hash_u32(&ctx, contract->fields[i].offset);
        hash_u32(&ctx, contract->fields[i].width);
    }
    for (uint32_t i = 0; i < XR_RUNTIME_STRING_MATERIALIZATION_COUNT;
         i++) {
        const XrRuntimeStringMaterializationDescriptor *materialization =
            &contract->materializations[i];
        hash_u32(&ctx, i);
        xr_sha256_update(&ctx, &materialization->kind, 1);
        xr_sha256_update(&ctx, &materialization->has_object_header, 1);
        xr_sha256_update(&ctx, &materialization->owns_utf8_bytes, 1);
        xr_sha256_update(&ctx, &materialization->nul_terminated, 1);
        hash_u32(&ctx, materialization->semantic_domain_mask);
        hash_u32(&ctx, materialization->backend_materialization_mask);
    }
    xr_sha256_update(&ctx, contract->literal_view.fingerprint.bytes,
                     sizeof(contract->literal_view.fingerprint.bytes));
    xr_sha256_update(&ctx, contract->extent.fingerprint.bytes,
                     sizeof(contract->extent.fingerprint.bytes));
    xr_sha256_update(&ctx, contract->layout.fingerprint.bytes,
                     sizeof(contract->layout.fingerprint.bytes));
    for (uint32_t i = 0; i < contract->domain_count; i++) {
        hash_u32(&ctx, i);
        hash_id(&ctx, contract->domains[i].contract_id);
        hash_u32(&ctx, contract->domains[i].instance_id);
        xr_sha256_update(&ctx, &contract->domains[i].semantic_domain, 1);
        xr_sha256_update(&ctx, &contract->domains[i].materialization, 1);
    }
    xr_sha256_final(&ctx, out->bytes);
    return XR_RUNTIME_ABI_OK;
}

XrRuntimeAbiStatus xr_runtime_string_object_contract_build(
    XrRuntimeStringObjectContract *out) {
    if (!out)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    XrRuntimeStringObjectContract candidate;
    XrRuntimeAbiStatus status = build_unfingerprinted(&candidate);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    status = xr_runtime_string_object_contract_fingerprint(
        &candidate, &candidate.fingerprint);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    *out = candidate;
    return XR_RUNTIME_ABI_OK;
}

XrRuntimeAbiStatus xr_runtime_string_object_contract_verify(
    const XrRuntimeStringObjectContract *contract) {
    XrRuntimeAbiStatus status = verify_shape(contract);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    XrRuntimeStringObjectContract expected;
    status = xr_runtime_string_object_contract_build(&expected);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    if (!bytes_equal(contract->extent.fingerprint.bytes,
                     expected.extent.fingerprint.bytes,
                     sizeof(contract->extent.fingerprint.bytes)) ||
        !bytes_equal(contract->layout.fingerprint.bytes,
                     expected.layout.fingerprint.bytes,
                     sizeof(contract->layout.fingerprint.bytes)))
        return XR_RUNTIME_ABI_INVALID_POLICY;
    for (uint32_t i = 0; i < contract->domain_count; i++) {
        if (!xr_runtime_domain_identity_equal(contract->domains[i],
                                              expected.domains[i]))
            return XR_RUNTIME_ABI_INVALID_POLICY;
    }
    XrFingerprint actual;
    status = xr_runtime_string_object_contract_fingerprint(contract, &actual);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    return bytes_equal(actual.bytes, contract->fingerprint.bytes,
                       sizeof(actual.bytes))
               ? XR_RUNTIME_ABI_OK
               : XR_RUNTIME_ABI_FINGERPRINT_MISMATCH;
}

XrRuntimeAbiStatus xr_runtime_string_object_extent_for_length(
    const XrRuntimeStringObjectContract *contract, uint32_t byte_length,
    XrRuntimeExtentLimits limits, XrRuntimeEvaluatedExtent *out) {
    if (!out)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    XrRuntimeAbiStatus status =
        xr_runtime_string_object_contract_verify(contract);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    if (byte_length > contract->maximum_byte_length)
        return XR_RUNTIME_ABI_LIMIT_EXCEEDED;
    uint64_t operand = (uint64_t) byte_length +
                       (uint64_t) contract->extent_operand_bias;
    if (operand < byte_length)
        return XR_RUNTIME_ABI_OVERFLOW;
    return xr_runtime_extent_evaluate(
        &contract->extent, &contract->layout, &operand, 1, limits, NULL,
        NULL, out);
}

XrRuntimeAbiStatus xr_runtime_string_object_validate_prefix(
    const XrString *string) {
    if (!string)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    XrRuntimeAbiStatus status =
        xr_runtime_object_header_validate(&string->header);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    if (string->header.object_kind != XR_RUNTIME_OBJECT_KIND_STRING ||
        string->header.layout_id != XR_RUNTIME_STRING_LAYOUT_INDEX ||
        string->header.domain_id >= XR_RUNTIME_STRING_DOMAIN_COUNT ||
        string->reserved16 != 0 ||
        (string->rune_length != UINT32_MAX &&
         string->rune_length > string->length))
        return XR_RUNTIME_ABI_INVALID_SHAPE;
    uint16_t traits =
        atomic_load_explicit(&string->traits, memory_order_relaxed);
    if (!xr_runtime_string_traits_valid(string->header.domain_id, traits))
        return XR_RUNTIME_ABI_INVALID_POLICY;
    if (string->length > XR_RUNTIME_STRING_MAXIMUM_BYTE_LENGTH)
        return XR_RUNTIME_ABI_LIMIT_EXCEEDED;
    return XR_RUNTIME_ABI_OK;
}

XrRuntimeAbiStatus xr_runtime_string_object_validate(
    const XrString *string, size_t allocation_size) {
    XrRuntimeAbiStatus status =
        xr_runtime_string_object_validate_prefix(string);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    size_t required = (size_t) xr_runtime_string_object_allocation_bytes(
        string->length);
    if (required != allocation_size || string->data[string->length] != '\0')
        return XR_RUNTIME_ABI_INVALID_EXTENT;
    return XR_RUNTIME_ABI_OK;
}
