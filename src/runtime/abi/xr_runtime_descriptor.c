/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_runtime_descriptor.c - Canonical runtime layout and extent validation
 */

#include "xr_runtime_descriptor.h"
#include "../../base/xsha256.h"
#include <limits.h>
#include <string.h>

static const uint8_t xr_runtime_abi_domain[] = "xray-runtime-abi-v1\0";

typedef enum XrRuntimeAbiRecordKind {
    XR_RUNTIME_ABI_RECORD_EXTENT = 1,
    XR_RUNTIME_ABI_RECORD_LAYOUT = 2,
    XR_RUNTIME_ABI_RECORD_EXTENT_GROUP = 3,
} XrRuntimeAbiRecordKind;

static bool id_is_zero(XrStableId id) {
    static const XrStableId zero = {{0}};
    return memcmp(id.bytes, zero.bytes, sizeof(id.bytes)) == 0;
}

static bool id_equal(XrStableId left, XrStableId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static bool fingerprint_equal(XrFingerprint left, XrFingerprint right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static bool is_power_of_two(uint32_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

static void hash_u8(XrSHA256Context *ctx, uint8_t value) {
    xr_sha256_update(ctx, &value, sizeof(value));
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

static void hash_u64(XrSHA256Context *ctx, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8));
    xr_sha256_update(ctx, bytes, sizeof(bytes));
}

static void hash_id(XrSHA256Context *ctx, XrStableId id) {
    xr_sha256_update(ctx, id.bytes, sizeof(id.bytes));
}

static void hash_begin(XrSHA256Context *ctx, XrRuntimeAbiRecordKind kind) {
    xr_sha256_init(ctx);
    xr_sha256_update(ctx, xr_runtime_abi_domain, sizeof(xr_runtime_abi_domain) - 1);
    hash_u8(ctx, (uint8_t) kind);
}

XrRuntimeAbiStatus xr_runtime_extent_descriptor_fingerprint(
    const XrRuntimeExtentDescriptor *extent, XrFingerprint *out) {
    if (!extent || !out)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    XrSHA256Context ctx;
    hash_begin(&ctx, XR_RUNTIME_ABI_RECORD_EXTENT);
    hash_u32(&ctx, extent->schema_version);
    hash_id(&ctx, extent->id);
    hash_id(&ctx, extent->layout_id);
    hash_id(&ctx, extent->group_id);
    hash_id(&ctx, extent->provider_id);
    hash_u64(&ctx, extent->tail_offset);
    hash_u64(&ctx, extent->stride);
    hash_u16(&ctx, extent->operand_index);
    hash_u16(&ctx, extent->part_index);
    hash_u16(&ctx, extent->part_count);
    hash_u8(&ctx, extent->kind);
    xr_sha256_final(&ctx, out->bytes);
    return XR_RUNTIME_ABI_OK;
}

static XrRuntimeAbiStatus verify_extent_shape(const XrRuntimeExtentDescriptor *extent,
                                              const XrRuntimeLayoutDescriptor *layout) {
    if (extent->schema_version != XR_RUNTIME_ABI_SCHEMA_VERSION ||
        layout->schema_version != XR_RUNTIME_ABI_SCHEMA_VERSION)
        return XR_RUNTIME_ABI_INVALID_SCHEMA;
    if (id_is_zero(extent->id) || id_is_zero(extent->layout_id) ||
        id_is_zero(layout->layout_id) || !id_equal(extent->layout_id, layout->layout_id))
        return XR_RUNTIME_ABI_INVALID_IDENTITY;
    if (extent->kind >= XR_RUNTIME_EXTENT_KIND_COUNT)
        return XR_RUNTIME_ABI_INVALID_KIND;
    if (!is_power_of_two(layout->alignment))
        return XR_RUNTIME_ABI_INVALID_ALIGNMENT;
    if (extent->part_count == 0 || extent->part_index >= extent->part_count)
        return XR_RUNTIME_ABI_INVALID_EXTENT;

    bool group_is_zero = id_is_zero(extent->group_id);
    bool provider_is_zero = id_is_zero(extent->provider_id);
    switch ((XrRuntimeExtentKind) extent->kind) {
        case XR_RUNTIME_EXTENT_FIXED:
            if (extent->operand_index != XR_RUNTIME_EXTENT_OPERAND_NONE ||
                extent->part_count != 1 ||
                extent->part_index != 0 || extent->stride != 0 || extent->tail_offset != 0 ||
                !group_is_zero ||
                !provider_is_zero)
                return XR_RUNTIME_ABI_INVALID_EXTENT;
            break;
        case XR_RUNTIME_EXTENT_EXTERNAL_BUFFER:
            if (extent->operand_index == XR_RUNTIME_EXTENT_OPERAND_NONE ||
                extent->part_count != 1 ||
                extent->part_index != 0 || extent->stride == 0 ||
                extent->tail_offset < layout->fixed_prefix_size || !group_is_zero ||
                !provider_is_zero)
                return XR_RUNTIME_ABI_INVALID_EXTENT;
            break;
        case XR_RUNTIME_EXTENT_INLINE_TAIL:
            if (extent->operand_index == XR_RUNTIME_EXTENT_OPERAND_NONE ||
                extent->part_count != 1 ||
                extent->part_index != 0 || extent->stride == 0 ||
                extent->tail_offset < layout->fixed_prefix_size || !group_is_zero ||
                !provider_is_zero)
                return XR_RUNTIME_ABI_INVALID_EXTENT;
            break;
        case XR_RUNTIME_EXTENT_MULTI_BUFFER:
            if (extent->operand_index == XR_RUNTIME_EXTENT_OPERAND_NONE ||
                extent->part_count < 2 ||
                extent->stride == 0 || extent->tail_offset < layout->fixed_prefix_size ||
                group_is_zero || !provider_is_zero)
                return XR_RUNTIME_ABI_INVALID_EXTENT;
            break;
        case XR_RUNTIME_EXTENT_PROVIDER_DEFINED:
            if (provider_is_zero || !group_is_zero || extent->part_count != 1 ||
                extent->part_index != 0 || extent->stride != 0 ||
                extent->tail_offset != 0 ||
                extent->operand_index != XR_RUNTIME_EXTENT_OPERAND_NONE)
                return XR_RUNTIME_ABI_INVALID_EXTENT;
            break;
        default:
            return XR_RUNTIME_ABI_INVALID_KIND;
    }
    return XR_RUNTIME_ABI_OK;
}

XrRuntimeAbiStatus xr_runtime_extent_descriptor_verify(
    const XrRuntimeExtentDescriptor *extent, const XrRuntimeLayoutDescriptor *layout) {
    if (!extent || !layout)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    XrRuntimeAbiStatus status = verify_extent_shape(extent, layout);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    XrFingerprint actual;
    status = xr_runtime_extent_descriptor_fingerprint(extent, &actual);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    return fingerprint_equal(actual, extent->fingerprint) ? XR_RUNTIME_ABI_OK
                                                          : XR_RUNTIME_ABI_FINGERPRINT_MISMATCH;
}

static XrRuntimeAbiStatus align_extent(uint64_t bytes, uint32_t alignment, uint64_t *out) {
    uint64_t mask = (uint64_t) alignment - 1;
    if (bytes > UINT64_MAX - mask)
        return XR_RUNTIME_ABI_OVERFLOW;
    *out = (bytes + mask) & ~mask;
    return XR_RUNTIME_ABI_OK;
}

static XrRuntimeAbiStatus evaluate_formula(const XrRuntimeExtentDescriptor *extent,
                                           const XrRuntimeLayoutDescriptor *layout,
                                           const uint64_t *operands, size_t operand_count,
                                           XrRuntimeExtentProviderEvaluateFn provider,
                                           void *provider_context, uint64_t *bytes,
                                           uint64_t *operand) {
    *operand = 0;
    if (extent->kind == XR_RUNTIME_EXTENT_PROVIDER_DEFINED) {
        if (!provider)
            return XR_RUNTIME_ABI_PROVIDER_REQUIRED;
        XrRuntimeAbiStatus status =
            provider(extent->provider_id, operands, operand_count, bytes, provider_context);
        if (status != XR_RUNTIME_ABI_OK)
            return XR_RUNTIME_ABI_PROVIDER_REJECTED;
        if (*bytes < layout->fixed_prefix_size)
            return XR_RUNTIME_ABI_INVALID_EXTENT;
        return XR_RUNTIME_ABI_OK;
    }
    if (extent->kind == XR_RUNTIME_EXTENT_FIXED) {
        *bytes = layout->fixed_prefix_size;
        return XR_RUNTIME_ABI_OK;
    }
    if (!operands || extent->operand_index >= operand_count)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    *operand = operands[extent->operand_index];
    if (*operand != 0 && extent->stride > UINT64_MAX / *operand)
        return XR_RUNTIME_ABI_OVERFLOW;
    uint64_t tail = *operand * extent->stride;
    if (extent->tail_offset > UINT64_MAX - tail)
        return XR_RUNTIME_ABI_OVERFLOW;
    *bytes = extent->tail_offset + tail;
    return XR_RUNTIME_ABI_OK;
}

XrRuntimeAbiStatus xr_runtime_extent_evaluate(
    const XrRuntimeExtentDescriptor *extent, const XrRuntimeLayoutDescriptor *layout,
    const uint64_t *operands, size_t operand_count, XrRuntimeExtentLimits limits,
    XrRuntimeExtentProviderEvaluateFn provider, void *provider_context,
    XrRuntimeEvaluatedExtent *out) {
    if (!out || limits.max_allocation_bytes == 0 || limits.max_alignment == 0 ||
        (operand_count != 0 && !operands))
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    XrRuntimeAbiStatus status = xr_runtime_layout_descriptor_verify(layout, extent);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    if (layout->alignment > limits.max_alignment)
        return XR_RUNTIME_ABI_LIMIT_EXCEEDED;

    uint64_t bytes = 0;
    uint64_t operand = 0;
    status = evaluate_formula(extent, layout, operands, operand_count, provider, provider_context,
                              &bytes, &operand);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    status = align_extent(bytes, layout->alignment, &bytes);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    if (bytes > limits.max_allocation_bytes)
        return XR_RUNTIME_ABI_LIMIT_EXCEEDED;

    *out = (XrRuntimeEvaluatedExtent) {.extent_id = extent->id,
                                .extent_fingerprint = extent->fingerprint,
                                .bytes = bytes,
                                .operand = operand,
                                .alignment = layout->alignment,
                                .part_index = extent->part_index,
                                .part_count = extent->part_count};
    return XR_RUNTIME_ABI_OK;
}

XrRuntimeAbiStatus xr_runtime_layout_descriptor_fingerprint(
    const XrRuntimeLayoutDescriptor *layout, XrFingerprint *out) {
    if (!layout || !out)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    XrSHA256Context ctx;
    hash_begin(&ctx, XR_RUNTIME_ABI_RECORD_LAYOUT);
    hash_u32(&ctx, layout->schema_version);
    hash_id(&ctx, layout->descriptor_id);
    hash_id(&ctx, layout->layout_id);
    hash_id(&ctx, layout->object_kind_id);
    hash_id(&ctx, layout->extent_id);
    hash_id(&ctx, layout->root_plan_id);
    hash_id(&ctx, layout->destructor_id);
    hash_id(&ctx, layout->clone_id);
    hash_id(&ctx, layout->eq_hash_id);
    xr_sha256_update(&ctx, layout->extent_fingerprint.bytes,
                     sizeof(layout->extent_fingerprint.bytes));
    hash_u64(&ctx, layout->fixed_prefix_size);
    hash_u32(&ctx, layout->alignment);
    hash_u32(&ctx, layout->allowed_semantic_domains);
    hash_u32(&ctx, layout->allowed_materializations);
    hash_u32(&ctx, layout->flags);
    xr_sha256_final(&ctx, out->bytes);
    return XR_RUNTIME_ABI_OK;
}

static const XrRuntimeExtentGroupEntry *find_group_part(
    const XrRuntimeExtentGroupEntry *entries, size_t entry_count, uint16_t part_index,
    size_t *match_count) {
    const XrRuntimeExtentGroupEntry *match = NULL;
    *match_count = 0;
    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i].extent && entries[i].extent->part_index == part_index) {
            match = &entries[i];
            (*match_count)++;
        }
    }
    return match;
}

static bool group_identities_unique(const XrRuntimeExtentGroupEntry *entries,
                                    size_t entry_count) {
    for (size_t i = 0; i < entry_count; i++) {
        for (size_t j = i + 1; j < entry_count; j++) {
            if (id_equal(entries[i].extent->id, entries[j].extent->id) ||
                id_equal(entries[i].layout->descriptor_id,
                         entries[j].layout->descriptor_id))
                return false;
        }
    }
    return true;
}

XrRuntimeAbiStatus xr_runtime_extent_group_verify(
    const XrRuntimeExtentGroupEntry *entries, size_t entry_count,
    XrRuntimeExtentGroupSummary *out) {
    if (!entries || !out)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    if (entry_count < 2 || entry_count > UINT16_MAX)
        return XR_RUNTIME_ABI_INVALID_GROUP;
    for (size_t i = 0; i < entry_count; i++) {
        if (!entries[i].extent || !entries[i].layout)
            return XR_RUNTIME_ABI_INVALID_ARGUMENT;
        XrRuntimeAbiStatus status =
            xr_runtime_layout_descriptor_verify(entries[i].layout, entries[i].extent);
        if (status != XR_RUNTIME_ABI_OK)
            return status;
        if (entries[i].extent->kind != XR_RUNTIME_EXTENT_MULTI_BUFFER ||
            entries[i].extent->part_count != entry_count ||
            !id_equal(entries[i].extent->group_id, entries[0].extent->group_id))
            return XR_RUNTIME_ABI_INVALID_GROUP;
    }
    if (!group_identities_unique(entries, entry_count))
        return XR_RUNTIME_ABI_INVALID_GROUP;

    XrSHA256Context ctx;
    hash_begin(&ctx, XR_RUNTIME_ABI_RECORD_EXTENT_GROUP);
    hash_id(&ctx, entries[0].extent->group_id);
    hash_u16(&ctx, (uint16_t) entry_count);
    for (uint16_t part_index = 0; part_index < entry_count; part_index++) {
        size_t match_count = 0;
        const XrRuntimeExtentGroupEntry *part =
            find_group_part(entries, entry_count, part_index, &match_count);
        if (!part || match_count != 1)
            return XR_RUNTIME_ABI_INVALID_GROUP;
        hash_u16(&ctx, part_index);
        hash_id(&ctx, part->extent->id);
        xr_sha256_update(&ctx, part->extent->fingerprint.bytes,
                         sizeof(part->extent->fingerprint.bytes));
        hash_id(&ctx, part->layout->descriptor_id);
        xr_sha256_update(&ctx, part->layout->fingerprint.bytes,
                         sizeof(part->layout->fingerprint.bytes));
    }

    XrRuntimeExtentGroupSummary summary = {.group_id = entries[0].extent->group_id,
                                           .part_count = (uint16_t) entry_count};
    xr_sha256_final(&ctx, summary.fingerprint.bytes);
    *out = summary;
    return XR_RUNTIME_ABI_OK;
}

static bool flag_identity_valid(uint32_t flags, uint32_t flag, XrStableId id) {
    return ((flags & flag) != 0) != id_is_zero(id);
}

XrRuntimeAbiStatus xr_runtime_layout_descriptor_verify(
    const XrRuntimeLayoutDescriptor *layout, const XrRuntimeExtentDescriptor *extent) {
    if (!layout || !extent)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    if (layout->schema_version != XR_RUNTIME_ABI_SCHEMA_VERSION)
        return XR_RUNTIME_ABI_INVALID_SCHEMA;
    if (id_is_zero(layout->descriptor_id) || id_is_zero(layout->layout_id) ||
        id_is_zero(layout->object_kind_id) || id_is_zero(layout->extent_id))
        return XR_RUNTIME_ABI_INVALID_IDENTITY;
    if (!is_power_of_two(layout->alignment))
        return XR_RUNTIME_ABI_INVALID_ALIGNMENT;
    if (layout->allowed_semantic_domains == 0 ||
        (layout->allowed_semantic_domains & ~XR_SEMANTIC_DOMAIN_MASK_ALL) != 0 ||
        layout->allowed_materializations == 0 ||
        (layout->allowed_materializations & ~XR_MATERIALIZATION_MASK_ALL) != 0)
        return XR_RUNTIME_ABI_INVALID_DOMAIN;
    if ((layout->flags & ~XR_LAYOUT_DESCRIPTOR_FLAGS_ALL) != 0 ||
        !flag_identity_valid(layout->flags, XR_LAYOUT_HAS_ROOTS, layout->root_plan_id) ||
        !flag_identity_valid(layout->flags, XR_LAYOUT_HAS_DESTRUCTOR,
                             layout->destructor_id) ||
        !flag_identity_valid(layout->flags, XR_LAYOUT_HAS_CLONE, layout->clone_id) ||
        !flag_identity_valid(layout->flags, XR_LAYOUT_HAS_EQ_HASH, layout->eq_hash_id))
        return XR_RUNTIME_ABI_INVALID_IDENTITY;

    XrRuntimeAbiStatus status = xr_runtime_extent_descriptor_verify(extent, layout);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    if (!id_equal(layout->extent_id, extent->id) ||
        !fingerprint_equal(layout->extent_fingerprint, extent->fingerprint))
        return XR_RUNTIME_ABI_INVALID_EXTENT;

    XrFingerprint actual;
    status = xr_runtime_layout_descriptor_fingerprint(layout, &actual);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    return fingerprint_equal(actual, layout->fingerprint)
               ? XR_RUNTIME_ABI_OK
               : XR_RUNTIME_ABI_FINGERPRINT_MISMATCH;
}

bool xr_runtime_layout_allows_domain(const XrRuntimeLayoutDescriptor *layout,
                                     XrRuntimeDomainIdentity domain) {
    if (!layout || !xr_runtime_domain_identity_valid(domain))
        return false;
    return (layout->allowed_semantic_domains & XR_SEMANTIC_DOMAIN_MASK(domain.semantic_domain)) !=
               0 &&
           (layout->allowed_materializations &
            XR_MATERIALIZATION_MASK(domain.materialization)) != 0;
}

bool xr_runtime_domain_identity_valid(XrRuntimeDomainIdentity domain) {
    return !id_is_zero(domain.contract_id) &&
           domain.instance_id != XR_RUNTIME_DOMAIN_INSTANCE_INVALID &&
           domain.semantic_domain > XR_STORAGE_DOMAIN_UNKNOWN &&
           domain.semantic_domain <= XR_STORAGE_FOREIGN &&
           domain.materialization > XR_MATERIALIZE_INVALID &&
           domain.materialization <= XR_MATERIALIZE_EXTERNAL;
}

bool xr_runtime_domain_identity_equal(XrRuntimeDomainIdentity left,
                                      XrRuntimeDomainIdentity right) {
    return id_equal(left.contract_id, right.contract_id) &&
           left.instance_id == right.instance_id &&
           left.semantic_domain == right.semantic_domain &&
           left.materialization == right.materialization;
}

const char *xr_runtime_abi_status_name(XrRuntimeAbiStatus status) {
    static const char *const names[] = {
        "ok",          "invalid-argument", "invalid-schema",   "invalid-identity",
        "invalid-kind", "invalid-alignment", "invalid-domain", "invalid-extent",
        "fingerprint-mismatch", "overflow", "limit-exceeded", "provider-required",
        "provider-rejected", "invalid-group", "invalid-shape", "invalid-order",
        "invalid-overlap", "invalid-mask", "invalid-policy", "invalid-provider-set",
        "budget-exceeded",
    };
    return status >= XR_RUNTIME_ABI_OK && status < XR_RUNTIME_ABI_STATUS_COUNT
               ? names[status]
               : "unknown-status";
}
