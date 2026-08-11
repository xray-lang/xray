/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_runtime_contract.c - Structured runtime ABI validation and fingerprints
 */

#include "xr_runtime_contract.h"
#include "../../base/xsha256.h"
#include <limits.h>
#include <stdbool.h>
#include <string.h>

static const uint8_t xr_runtime_contract_domain[] = "xray-runtime-abi-v1\0";

typedef enum XrRuntimeContractRecordKind {
    XR_RUNTIME_CONTRACT_OBJECT_HEADER = 4,
    XR_RUNTIME_CONTRACT_WHOLE_RUNTIME = 5,
    XR_RUNTIME_CONTRACT_PROVIDER_CALL_ABI = 6,
    XR_RUNTIME_CONTRACT_PROVIDER_SET = 7,
} XrRuntimeContractRecordKind;

static bool bytes_are_zero(const uint8_t *bytes, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (bytes[i] != 0)
            return false;
    }
    return true;
}

static bool id_is_zero(XrStableId id) {
    return bytes_are_zero(id.bytes, sizeof(id.bytes));
}

static int id_compare(XrStableId left, XrStableId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes));
}

static bool is_power_of_two_u64(uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

static bool scalar_width_valid(uint8_t width) {
    return width == 1 || width == 2 || width == 4 || width == 8;
}

static bool value_fits_width(uint64_t value, uint8_t width) {
    if (!scalar_width_valid(width))
        return false;
    return width == 8 || value < (UINT64_C(1) << (width * 8));
}

static uint64_t width_mask(uint8_t width) {
    return width == 8 ? UINT64_MAX : (UINT64_C(1) << (width * 8)) - UINT64_C(1);
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

static void hash_i32(XrSHA256Context *ctx, int32_t value) {
    hash_u32(ctx, (uint32_t) value);
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

static void hash_fingerprint(XrSHA256Context *ctx, XrFingerprint fingerprint) {
    xr_sha256_update(ctx, fingerprint.bytes, sizeof(fingerprint.bytes));
}

static void hash_begin(XrSHA256Context *ctx, XrRuntimeContractRecordKind kind) {
    xr_sha256_init(ctx);
    xr_sha256_update(ctx, xr_runtime_contract_domain,
                     sizeof(xr_runtime_contract_domain) - 1);
    hash_u8(ctx, (uint8_t) kind);
}

static bool field_is_zero(const XrRuntimePhysicalFieldAbi *field) {
    return field->role == 0 && field->offset == 0 && field->width == 0 &&
           field->alignment == 0 && field->encoding == 0 && field->atomicity == 0 &&
           field->index_semantics == 0 && field->reserved8 == 0 &&
           field->reserved32 == 0;
}

static XrRuntimeAbiStatus verify_field(const XrRuntimePhysicalFieldAbi *field,
                                       uint16_t record_size, uint16_t record_alignment) {
    if (field->role <= XR_RUNTIME_FIELD_ROLE_INVALID ||
        field->role >= XR_RUNTIME_FIELD_ROLE_COUNT || field->width == 0 ||
        !is_power_of_two_u64(field->alignment) || field->alignment > field->width ||
        field->alignment > record_alignment || field->offset % field->alignment != 0 ||
        (uint32_t) field->offset + field->width > record_size)
        return XR_RUNTIME_ABI_INVALID_SHAPE;
    if (field->reserved8 != 0 || field->reserved32 != 0)
        return XR_RUNTIME_ABI_INVALID_POLICY;
    if (field->atomicity != XR_RUNTIME_FIELD_PLAIN &&
        field->atomicity != XR_RUNTIME_FIELD_DOMAIN_CONDITIONAL)
        return XR_RUNTIME_ABI_INVALID_POLICY;
    if (field->index_semantics > XR_RUNTIME_INDEX_VERIFIED_TABLE)
        return XR_RUNTIME_ABI_INVALID_POLICY;

    switch ((XrRuntimeFieldEncoding) field->encoding) {
        case XR_RUNTIME_FIELD_UNSIGNED:
        case XR_RUNTIME_FIELD_SIGNED_TWOS_COMPLEMENT:
        case XR_RUNTIME_FIELD_BITSET:
            if (!scalar_width_valid((uint8_t) field->width))
                return XR_RUNTIME_ABI_INVALID_SHAPE;
            break;
        case XR_RUNTIME_FIELD_STABLE_ID:
            if (field->width != XR_STABLE_ID_BYTES)
                return XR_RUNTIME_ABI_INVALID_SHAPE;
            break;
        case XR_RUNTIME_FIELD_FINGERPRINT:
            if (field->width != XR_FINGERPRINT_BYTES)
                return XR_RUNTIME_ABI_INVALID_SHAPE;
            break;
        case XR_RUNTIME_FIELD_OPAQUE_BITS:
            break;
        default:
            return XR_RUNTIME_ABI_INVALID_POLICY;
    }
    if (field->index_semantics != XR_RUNTIME_INDEX_NONE &&
        field->encoding != XR_RUNTIME_FIELD_UNSIGNED)
        return XR_RUNTIME_ABI_INVALID_POLICY;
    if (field->atomicity == XR_RUNTIME_FIELD_DOMAIN_CONDITIONAL &&
        field->encoding != XR_RUNTIME_FIELD_SIGNED_TWOS_COMPLEMENT)
        return XR_RUNTIME_ABI_INVALID_POLICY;
    return XR_RUNTIME_ABI_OK;
}

static XrRuntimeAbiStatus verify_field_sequence(
    const XrRuntimePhysicalFieldAbi *fields, size_t field_count, uint16_t record_size,
    uint16_t record_alignment, const uint16_t *expected_roles, size_t expected_count) {
    if (!fields || !expected_roles || field_count != expected_count || record_size == 0 ||
        !is_power_of_two_u64(record_alignment) || record_size % record_alignment != 0)
        return XR_RUNTIME_ABI_INVALID_SHAPE;
    uint32_t previous_end = 0;
    for (size_t i = 0; i < field_count; i++) {
        XrRuntimeAbiStatus status = verify_field(&fields[i], record_size, record_alignment);
        if (status != XR_RUNTIME_ABI_OK)
            return status;
        if (fields[i].role != expected_roles[i])
            return XR_RUNTIME_ABI_INVALID_ORDER;
        if (i == 0 && fields[i].offset != 0)
            return XR_RUNTIME_ABI_INVALID_SHAPE;
        if (i != 0 && fields[i].offset < fields[i - 1].offset)
            return XR_RUNTIME_ABI_INVALID_ORDER;
        if (fields[i].offset < previous_end)
            return XR_RUNTIME_ABI_INVALID_OVERLAP;
        previous_end = (uint32_t) fields[i].offset + fields[i].width;
    }
    return XR_RUNTIME_ABI_OK;
}

static void hash_field(XrSHA256Context *ctx, const XrRuntimePhysicalFieldAbi *field) {
    hash_u16(ctx, field->role);
    hash_u16(ctx, field->offset);
    hash_u16(ctx, field->width);
    hash_u16(ctx, field->alignment);
    hash_u8(ctx, field->encoding);
    hash_u8(ctx, field->atomicity);
    hash_u8(ctx, field->index_semantics);
}

static bool kind_entry_is_zero(const XrRuntimeObjectKindAbiEntry *entry) {
    return id_is_zero(entry->stable_id) && entry->encoding == 0 && entry->reserved == 0;
}

static XrRuntimeAbiStatus verify_object_kinds(const XrRuntimeObjectKindAbi *kinds) {
    if (kinds->entry_count > XR_RUNTIME_ABI_MAX_OBJECT_KINDS)
        return XR_RUNTIME_ABI_BUDGET_EXCEEDED;
    if (kinds->entry_count == 0 || !scalar_width_valid(kinds->encoding_width) ||
        !value_fits_width(kinds->invalid_encoding, kinds->encoding_width))
        return XR_RUNTIME_ABI_INVALID_SHAPE;
    if (kinds->reserved8 != 0 || kinds->reserved32 != 0)
        return XR_RUNTIME_ABI_INVALID_POLICY;
    for (size_t i = 0; i < kinds->entry_count; i++) {
        const XrRuntimeObjectKindAbiEntry *entry = &kinds->entries[i];
        if (id_is_zero(entry->stable_id) || entry->reserved != 0 ||
            !value_fits_width(entry->encoding, kinds->encoding_width) ||
            entry->encoding == kinds->invalid_encoding)
            return XR_RUNTIME_ABI_INVALID_SHAPE;
        if (i != 0 && id_compare(kinds->entries[i - 1].stable_id, entry->stable_id) >= 0)
            return XR_RUNTIME_ABI_INVALID_ORDER;
        for (size_t j = 0; j < i; j++) {
            if (kinds->entries[j].encoding == entry->encoding)
                return XR_RUNTIME_ABI_INVALID_ORDER;
        }
    }
    for (size_t i = kinds->entry_count; i < XR_RUNTIME_ABI_MAX_OBJECT_KINDS; i++) {
        if (!kind_entry_is_zero(&kinds->entries[i]))
            return XR_RUNTIME_ABI_INVALID_POLICY;
    }
    return XR_RUNTIME_ABI_OK;
}

static bool flag_entry_is_zero(const XrRuntimeObjectFlagAbiEntry *entry) {
    return id_is_zero(entry->stable_id) && entry->bit == 0 &&
           entry->exclusivity_group == 0 && entry->reserved16 == 0 &&
           entry->reserved32 == 0;
}

static XrRuntimeAbiStatus verify_flag_groups(const XrRuntimeObjectFlagAbi *flags) {
    uint16_t max_group = 0;
    for (size_t i = 0; i < flags->entry_count; i++) {
        if (flags->entries[i].exclusivity_group > max_group)
            max_group = flags->entries[i].exclusivity_group;
    }
    for (uint16_t group = 1; group <= max_group; group++) {
        size_t members = 0;
        for (size_t i = 0; i < flags->entry_count; i++) {
            if (flags->entries[i].exclusivity_group == group)
                members++;
        }
        if (members < 2)
            return XR_RUNTIME_ABI_INVALID_SHAPE;
    }
    return XR_RUNTIME_ABI_OK;
}

static XrRuntimeAbiStatus verify_object_flags(const XrRuntimeObjectFlagAbi *flags) {
    if (flags->entry_count > XR_RUNTIME_ABI_MAX_OBJECT_FLAGS)
        return XR_RUNTIME_ABI_BUDGET_EXCEEDED;
    if (!scalar_width_valid(flags->encoding_width))
        return XR_RUNTIME_ABI_INVALID_SHAPE;
    if (flags->reserved8 != 0 || flags->reserved32 != 0)
        return XR_RUNTIME_ABI_INVALID_POLICY;
    uint64_t complete_mask = width_mask(flags->encoding_width);
    if ((flags->valid_mask & flags->reserved_zero_mask) != 0 ||
        (flags->valid_mask | flags->reserved_zero_mask) != complete_mask)
        return XR_RUNTIME_ABI_INVALID_MASK;

    uint64_t observed_mask = 0;
    for (size_t i = 0; i < flags->entry_count; i++) {
        const XrRuntimeObjectFlagAbiEntry *entry = &flags->entries[i];
        if (id_is_zero(entry->stable_id) || entry->reserved16 != 0 ||
            entry->reserved32 != 0 || !is_power_of_two_u64(entry->bit) ||
            (entry->bit & complete_mask) != entry->bit || (observed_mask & entry->bit) != 0)
            return XR_RUNTIME_ABI_INVALID_SHAPE;
        if (i != 0 && id_compare(flags->entries[i - 1].stable_id, entry->stable_id) >= 0)
            return XR_RUNTIME_ABI_INVALID_ORDER;
        observed_mask |= entry->bit;
    }
    if (observed_mask != flags->valid_mask)
        return XR_RUNTIME_ABI_INVALID_MASK;
    XrRuntimeAbiStatus status = verify_flag_groups(flags);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    for (size_t i = flags->entry_count; i < XR_RUNTIME_ABI_MAX_OBJECT_FLAGS; i++) {
        if (!flag_entry_is_zero(&flags->entries[i]))
            return XR_RUNTIME_ABI_INVALID_POLICY;
    }
    return XR_RUNTIME_ABI_OK;
}

static XrRuntimeAbiStatus verify_table_index(const XrRuntimeTableIndexAbi *index) {
    if (!scalar_width_valid(index->encoding_width) ||
        !value_fits_width(index->invalid_encoding, index->encoding_width))
        return XR_RUNTIME_ABI_INVALID_SHAPE;
    if (index->semantics != XR_RUNTIME_INDEX_VERIFIED_TABLE || index->reserved16 != 0 ||
        index->reserved32 != 0)
        return XR_RUNTIME_ABI_INVALID_POLICY;
    return XR_RUNTIME_ABI_OK;
}

static XrRuntimeAbiStatus verify_rc(const XrRuntimeRcAbi *rc) {
    if (rc->initial_value <= 0 || rc->retain_delta <= 0 || rc->release_delta >= 0 ||
        rc->sticky_sentinel >= 0 || rc->sticky_band_boundary >= 0 ||
        rc->sticky_sentinel > rc->sticky_band_boundary)
        return XR_RUNTIME_ABI_INVALID_POLICY;
    if (rc->polarity != XR_RUNTIME_RC_OWNED_POSITIVE ||
        rc->sticky_comparison != XR_RUNTIME_RC_STICKY_LESS_OR_EQUAL ||
        rc->local_access != XR_RUNTIME_RC_ACCESS_PLAIN ||
        rc->shared_access != XR_RUNTIME_RC_ACCESS_ATOMIC ||
        rc->shared_retain_order != XR_RUNTIME_MEMORY_ORDER_RELAXED ||
        rc->shared_release_order != XR_RUNTIME_MEMORY_ORDER_RELEASE ||
        rc->shared_destroy_order != XR_RUNTIME_MEMORY_ORDER_ACQUIRE || rc->reserved8 != 0 ||
        rc->reserved32 != 0)
        return XR_RUNTIME_ABI_INVALID_POLICY;
    return XR_RUNTIME_ABI_OK;
}

static const uint16_t object_header_roles[XR_RUNTIME_OBJECT_HEADER_FIELD_COUNT] = {
    XR_RUNTIME_FIELD_HEADER_RC,
    XR_RUNTIME_FIELD_HEADER_OBJECT_KIND,
    XR_RUNTIME_FIELD_HEADER_FLAGS,
    XR_RUNTIME_FIELD_HEADER_LAYOUT_ID,
    XR_RUNTIME_FIELD_HEADER_DOMAIN_ID,
};

static XrRuntimeAbiStatus verify_object_header(const XrRuntimeObjectHeaderAbi *abi) {
    if (!abi)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    if (abi->schema_version != XR_RUNTIME_ABI_SCHEMA_VERSION)
        return XR_RUNTIME_ABI_INVALID_SCHEMA;
    if (abi->target_endian != XR_RUNTIME_ENDIAN_LITTLE &&
        abi->target_endian != XR_RUNTIME_ENDIAN_BIG)
        return XR_RUNTIME_ABI_INVALID_POLICY;
    if (abi->padding_policy != XR_RUNTIME_PADDING_MUST_BE_ZERO || abi->reserved16 != 0 ||
        abi->reserved32 != 0 || abi->reserved[0] != 0 || abi->reserved[1] != 0)
        return XR_RUNTIME_ABI_INVALID_POLICY;
    XrRuntimeAbiStatus status = verify_field_sequence(
        abi->fields, XR_RUNTIME_OBJECT_HEADER_FIELD_COUNT, abi->size, abi->alignment,
        object_header_roles, XR_RUNTIME_OBJECT_HEADER_FIELD_COUNT);
    if (status != XR_RUNTIME_ABI_OK)
        return status;

    const XrRuntimePhysicalFieldAbi *rc = &abi->fields[0];
    const XrRuntimePhysicalFieldAbi *kind = &abi->fields[1];
    const XrRuntimePhysicalFieldAbi *flags = &abi->fields[2];
    const XrRuntimePhysicalFieldAbi *layout = &abi->fields[3];
    const XrRuntimePhysicalFieldAbi *domain = &abi->fields[4];
    if (rc->width != 4 || rc->encoding != XR_RUNTIME_FIELD_SIGNED_TWOS_COMPLEMENT ||
        rc->atomicity != XR_RUNTIME_FIELD_DOMAIN_CONDITIONAL ||
        rc->index_semantics != XR_RUNTIME_INDEX_NONE ||
        kind->encoding != XR_RUNTIME_FIELD_UNSIGNED ||
        flags->encoding != XR_RUNTIME_FIELD_BITSET ||
        layout->encoding != XR_RUNTIME_FIELD_UNSIGNED ||
        domain->encoding != XR_RUNTIME_FIELD_UNSIGNED ||
        kind->atomicity != XR_RUNTIME_FIELD_PLAIN ||
        flags->atomicity != XR_RUNTIME_FIELD_PLAIN ||
        layout->atomicity != XR_RUNTIME_FIELD_PLAIN ||
        domain->atomicity != XR_RUNTIME_FIELD_PLAIN ||
        kind->index_semantics != XR_RUNTIME_INDEX_NONE ||
        flags->index_semantics != XR_RUNTIME_INDEX_NONE ||
        layout->index_semantics != XR_RUNTIME_INDEX_VERIFIED_TABLE ||
        domain->index_semantics != XR_RUNTIME_INDEX_VERIFIED_TABLE)
        return XR_RUNTIME_ABI_INVALID_POLICY;

    status = verify_rc(&abi->rc);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    status = verify_object_kinds(&abi->object_kinds);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    status = verify_object_flags(&abi->flags);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    status = verify_table_index(&abi->layout_id);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    status = verify_table_index(&abi->domain_id);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    if (kind->width != abi->object_kinds.encoding_width ||
        flags->width != abi->flags.encoding_width ||
        layout->width != abi->layout_id.encoding_width ||
        domain->width != abi->domain_id.encoding_width)
        return XR_RUNTIME_ABI_INVALID_SHAPE;
    return XR_RUNTIME_ABI_OK;
}

static void hash_object_header(XrSHA256Context *ctx, const XrRuntimeObjectHeaderAbi *abi) {
    hash_u32(ctx, abi->schema_version);
    hash_u16(ctx, abi->size);
    hash_u16(ctx, abi->alignment);
    hash_u8(ctx, abi->target_endian);
    hash_u8(ctx, abi->padding_policy);
    for (size_t i = 0; i < XR_RUNTIME_OBJECT_HEADER_FIELD_COUNT; i++)
        hash_field(ctx, &abi->fields[i]);
    hash_i32(ctx, abi->rc.initial_value);
    hash_i32(ctx, abi->rc.retain_delta);
    hash_i32(ctx, abi->rc.release_delta);
    hash_i32(ctx, abi->rc.sticky_sentinel);
    hash_i32(ctx, abi->rc.sticky_band_boundary);
    hash_u8(ctx, abi->rc.polarity);
    hash_u8(ctx, abi->rc.sticky_comparison);
    hash_u8(ctx, abi->rc.local_access);
    hash_u8(ctx, abi->rc.shared_access);
    hash_u8(ctx, abi->rc.shared_retain_order);
    hash_u8(ctx, abi->rc.shared_release_order);
    hash_u8(ctx, abi->rc.shared_destroy_order);
    hash_u64(ctx, abi->object_kinds.invalid_encoding);
    hash_u8(ctx, abi->object_kinds.encoding_width);
    hash_u16(ctx, abi->object_kinds.entry_count);
    for (size_t i = 0; i < abi->object_kinds.entry_count; i++) {
        hash_id(ctx, abi->object_kinds.entries[i].stable_id);
        hash_u64(ctx, abi->object_kinds.entries[i].encoding);
    }
    hash_u8(ctx, abi->flags.encoding_width);
    hash_u64(ctx, abi->flags.valid_mask);
    hash_u64(ctx, abi->flags.reserved_zero_mask);
    hash_u16(ctx, abi->flags.entry_count);
    for (size_t i = 0; i < abi->flags.entry_count; i++) {
        hash_id(ctx, abi->flags.entries[i].stable_id);
        hash_u64(ctx, abi->flags.entries[i].bit);
        hash_u16(ctx, abi->flags.entries[i].exclusivity_group);
    }
    hash_u8(ctx, abi->layout_id.encoding_width);
    hash_u8(ctx, abi->layout_id.semantics);
    hash_u64(ctx, abi->layout_id.invalid_encoding);
    hash_u8(ctx, abi->domain_id.encoding_width);
    hash_u8(ctx, abi->domain_id.semantics);
    hash_u64(ctx, abi->domain_id.invalid_encoding);
}

XrRuntimeAbiStatus xr_runtime_object_header_abi_fingerprint(
    const XrRuntimeObjectHeaderAbi *abi, XrFingerprint *out) {
    if (!out)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    XrRuntimeAbiStatus status = verify_object_header(abi);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    XrSHA256Context ctx;
    hash_begin(&ctx, XR_RUNTIME_CONTRACT_OBJECT_HEADER);
    hash_object_header(&ctx, abi);
    XrFingerprint fingerprint;
    xr_sha256_final(&ctx, fingerprint.bytes);
    *out = fingerprint;
    return XR_RUNTIME_ABI_OK;
}

static bool dynamic_tag_is_zero(const XrRuntimeDynamicTagAbiEntry *tag) {
    return id_is_zero(tag->stable_id) && tag->encoding == 0 && tag->required_flags == 0 &&
           tag->allowed_flags == 0 && tag->payload_kind == 0 &&
           bytes_are_zero(tag->reserved8, sizeof(tag->reserved8));
}

static const uint16_t dynamic_value_roles[XR_RUNTIME_DYNAMIC_FIELD_COUNT] = {
    XR_RUNTIME_FIELD_DYN_TAG,
    XR_RUNTIME_FIELD_DYN_FLAGS,
    XR_RUNTIME_FIELD_DYN_PAYLOAD,
};

static XrRuntimeAbiStatus verify_dynamic_tags(const XrRuntimeDynamicValueAbi *abi) {
    if (abi->tag_count > XR_RUNTIME_ABI_MAX_DYNAMIC_TAGS)
        return XR_RUNTIME_ABI_BUDGET_EXCEEDED;
    if (abi->tag_count < 2 || !scalar_width_valid(abi->tag_encoding_width) ||
        !scalar_width_valid(abi->flags_encoding_width) ||
        !scalar_width_valid(abi->object_reference_width))
        return XR_RUNTIME_ABI_INVALID_SHAPE;
    if (!value_fits_width(abi->invalid_tag, abi->tag_encoding_width) ||
        !value_fits_width(abi->null_tag, abi->tag_encoding_width) ||
        !value_fits_width(abi->object_reference_tag, abi->tag_encoding_width) ||
        abi->invalid_tag == abi->null_tag || abi->invalid_tag == abi->object_reference_tag ||
        abi->null_tag == abi->object_reference_tag)
        return XR_RUNTIME_ABI_INVALID_SHAPE;
    uint64_t flag_mask = width_mask(abi->flags_encoding_width);
    if ((abi->valid_flags_mask & abi->reserved_zero_mask) != 0 ||
        (abi->valid_flags_mask | abi->reserved_zero_mask) != flag_mask)
        return XR_RUNTIME_ABI_INVALID_MASK;

    bool saw_null = false;
    bool saw_object = false;
    for (size_t i = 0; i < abi->tag_count; i++) {
        const XrRuntimeDynamicTagAbiEntry *tag = &abi->tags[i];
        if (id_is_zero(tag->stable_id) ||
            !bytes_are_zero(tag->reserved8, sizeof(tag->reserved8)) ||
            !value_fits_width(tag->encoding, abi->tag_encoding_width) ||
            tag->encoding == abi->invalid_tag ||
            tag->payload_kind <= XR_RUNTIME_DYN_PAYLOAD_INVALID ||
            tag->payload_kind > XR_RUNTIME_DYN_PAYLOAD_OBJECT_REFERENCE ||
            (tag->required_flags & ~tag->allowed_flags) != 0 ||
            (tag->allowed_flags & ~abi->valid_flags_mask) != 0)
            return XR_RUNTIME_ABI_INVALID_SHAPE;
        if (i != 0 && id_compare(abi->tags[i - 1].stable_id, tag->stable_id) >= 0)
            return XR_RUNTIME_ABI_INVALID_ORDER;
        for (size_t j = 0; j < i; j++) {
            if (abi->tags[j].encoding == tag->encoding)
                return XR_RUNTIME_ABI_INVALID_ORDER;
        }
        if (tag->encoding == abi->null_tag) {
            saw_null = true;
            if (tag->payload_kind != XR_RUNTIME_DYN_PAYLOAD_NONE ||
                tag->required_flags != 0 || tag->allowed_flags != 0)
                return XR_RUNTIME_ABI_INVALID_POLICY;
        }
        if (tag->encoding == abi->object_reference_tag) {
            saw_object = true;
            if (tag->payload_kind != XR_RUNTIME_DYN_PAYLOAD_OBJECT_REFERENCE)
                return XR_RUNTIME_ABI_INVALID_POLICY;
        }
    }
    if (!saw_null || !saw_object)
        return XR_RUNTIME_ABI_INVALID_SHAPE;
    for (size_t i = abi->tag_count; i < XR_RUNTIME_ABI_MAX_DYNAMIC_TAGS; i++) {
        if (!dynamic_tag_is_zero(&abi->tags[i]))
            return XR_RUNTIME_ABI_INVALID_POLICY;
    }
    return XR_RUNTIME_ABI_OK;
}

static XrRuntimeAbiStatus verify_dynamic_value(const XrRuntimeDynamicValueAbi *abi,
                                               uint16_t pointer_width,
                                               uint8_t target_endian) {
    if (abi->schema_version != XR_RUNTIME_ABI_SCHEMA_VERSION)
        return XR_RUNTIME_ABI_INVALID_SCHEMA;
    if (abi->target_endian != target_endian ||
        abi->padding_policy != XR_RUNTIME_PADDING_MUST_BE_ZERO ||
        abi->object_reference_width != pointer_width || abi->reserved16 != 0 ||
        abi->reserved32 != 0 || !bytes_are_zero(abi->reserved8, sizeof(abi->reserved8)) ||
        abi->reserved[0] != 0 || abi->reserved[1] != 0)
        return XR_RUNTIME_ABI_INVALID_POLICY;
    XrRuntimeAbiStatus status = verify_field_sequence(
        abi->fields, XR_RUNTIME_DYNAMIC_FIELD_COUNT, abi->size, abi->alignment,
        dynamic_value_roles, XR_RUNTIME_DYNAMIC_FIELD_COUNT);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    const XrRuntimePhysicalFieldAbi *tag = &abi->fields[0];
    const XrRuntimePhysicalFieldAbi *flags = &abi->fields[1];
    const XrRuntimePhysicalFieldAbi *payload = &abi->fields[2];
    if (tag->encoding != XR_RUNTIME_FIELD_UNSIGNED ||
        flags->encoding != XR_RUNTIME_FIELD_BITSET ||
        payload->encoding != XR_RUNTIME_FIELD_OPAQUE_BITS ||
        tag->atomicity != XR_RUNTIME_FIELD_PLAIN ||
        flags->atomicity != XR_RUNTIME_FIELD_PLAIN ||
        payload->atomicity != XR_RUNTIME_FIELD_PLAIN ||
        tag->index_semantics != XR_RUNTIME_INDEX_NONE ||
        flags->index_semantics != XR_RUNTIME_INDEX_NONE ||
        payload->index_semantics != XR_RUNTIME_INDEX_NONE ||
        tag->width != abi->tag_encoding_width ||
        flags->width != abi->flags_encoding_width ||
        payload->width < abi->object_reference_width)
        return XR_RUNTIME_ABI_INVALID_POLICY;
    return verify_dynamic_tags(abi);
}

static void hash_dynamic_value(XrSHA256Context *ctx, const XrRuntimeDynamicValueAbi *abi) {
    hash_u32(ctx, abi->schema_version);
    hash_u16(ctx, abi->size);
    hash_u16(ctx, abi->alignment);
    hash_u8(ctx, abi->target_endian);
    hash_u8(ctx, abi->padding_policy);
    hash_u8(ctx, abi->tag_encoding_width);
    hash_u8(ctx, abi->flags_encoding_width);
    hash_u8(ctx, abi->object_reference_width);
    hash_u64(ctx, abi->invalid_tag);
    hash_u64(ctx, abi->null_tag);
    hash_u64(ctx, abi->object_reference_tag);
    hash_u64(ctx, abi->valid_flags_mask);
    hash_u64(ctx, abi->reserved_zero_mask);
    hash_u16(ctx, abi->tag_count);
    for (size_t i = 0; i < XR_RUNTIME_DYNAMIC_FIELD_COUNT; i++)
        hash_field(ctx, &abi->fields[i]);
    for (size_t i = 0; i < abi->tag_count; i++) {
        hash_id(ctx, abi->tags[i].stable_id);
        hash_u64(ctx, abi->tags[i].encoding);
        hash_u64(ctx, abi->tags[i].required_flags);
        hash_u64(ctx, abi->tags[i].allowed_flags);
        hash_u8(ctx, abi->tags[i].payload_kind);
    }
}

static bool enum_value_is_zero(const XrRuntimeEnumValueAbi *entry) {
    return id_is_zero(entry->stable_id) && entry->encoding == 0 && entry->reserved == 0;
}

static bool namespace_is_zero(const XrRuntimeEnumNamespaceAbi *space) {
    if (space->invalid_encoding != 0 || space->valid_mask != 0 ||
        space->reserved_zero_mask != 0 || space->role != 0 || space->entry_count != 0 ||
        space->kind != 0 || space->encoding_width != 0 || space->reserved16 != 0 ||
        space->reserved32 != 0)
        return false;
    for (size_t i = 0; i < XR_RUNTIME_ABI_MAX_ENUM_VALUES; i++) {
        if (!enum_value_is_zero(&space->entries[i]))
            return false;
    }
    return true;
}

static XrRuntimeAbiStatus verify_namespace_entries(
    const XrRuntimeEnumNamespaceAbi *space) {
    uint64_t observed_mask = 0;
    for (size_t i = 0; i < space->entry_count; i++) {
        const XrRuntimeEnumValueAbi *entry = &space->entries[i];
        if (id_is_zero(entry->stable_id) || entry->reserved != 0 ||
            !value_fits_width(entry->encoding, space->encoding_width) ||
            entry->encoding == space->invalid_encoding)
            return XR_RUNTIME_ABI_INVALID_SHAPE;
        if (i != 0 && id_compare(space->entries[i - 1].stable_id, entry->stable_id) >= 0)
            return XR_RUNTIME_ABI_INVALID_ORDER;
        for (size_t j = 0; j < i; j++) {
            if (space->entries[j].encoding == entry->encoding)
                return XR_RUNTIME_ABI_INVALID_ORDER;
        }
        if (space->kind == XR_RUNTIME_NAMESPACE_BITMASK) {
            if (!is_power_of_two_u64(entry->encoding) ||
                (observed_mask & entry->encoding) != 0)
                return XR_RUNTIME_ABI_INVALID_MASK;
            observed_mask |= entry->encoding;
        }
    }
    if (space->kind == XR_RUNTIME_NAMESPACE_BITMASK && observed_mask != space->valid_mask)
        return XR_RUNTIME_ABI_INVALID_MASK;
    for (size_t i = space->entry_count; i < XR_RUNTIME_ABI_MAX_ENUM_VALUES; i++) {
        if (!enum_value_is_zero(&space->entries[i]))
            return XR_RUNTIME_ABI_INVALID_POLICY;
    }
    return XR_RUNTIME_ABI_OK;
}

static XrRuntimeAbiStatus verify_namespace(const XrRuntimeEnumNamespaceAbi *space,
                                           uint16_t expected_role) {
    if (space->entry_count > XR_RUNTIME_ABI_MAX_ENUM_VALUES)
        return XR_RUNTIME_ABI_BUDGET_EXCEEDED;
    if (space->role != expected_role || !scalar_width_valid(space->encoding_width) ||
        !value_fits_width(space->invalid_encoding, space->encoding_width))
        return XR_RUNTIME_ABI_INVALID_SHAPE;
    if (space->reserved16 != 0 || space->reserved32 != 0)
        return XR_RUNTIME_ABI_INVALID_POLICY;

    uint64_t complete_mask = width_mask(space->encoding_width);
    switch ((XrRuntimeEnumNamespaceKind) space->kind) {
        case XR_RUNTIME_NAMESPACE_ENUM:
            if (space->entry_count == 0 || space->valid_mask != 0 ||
                space->reserved_zero_mask != 0)
                return XR_RUNTIME_ABI_INVALID_MASK;
            break;
        case XR_RUNTIME_NAMESPACE_BITMASK:
            if (space->entry_count == 0 || space->invalid_encoding != 0 ||
                (space->valid_mask & space->reserved_zero_mask) != 0 ||
                (space->valid_mask | space->reserved_zero_mask) != complete_mask)
                return XR_RUNTIME_ABI_INVALID_MASK;
            break;
        case XR_RUNTIME_NAMESPACE_SENTINEL:
            if (space->entry_count != 0 || space->valid_mask != 0 ||
                space->reserved_zero_mask != 0)
                return XR_RUNTIME_ABI_INVALID_MASK;
            break;
        default:
            return XR_RUNTIME_ABI_INVALID_POLICY;
    }
    return verify_namespace_entries(space);
}

static void hash_namespace(XrSHA256Context *ctx,
                           const XrRuntimeEnumNamespaceAbi *space) {
    hash_u16(ctx, space->role);
    hash_u8(ctx, space->kind);
    hash_u8(ctx, space->encoding_width);
    hash_u64(ctx, space->invalid_encoding);
    hash_u64(ctx, space->valid_mask);
    hash_u64(ctx, space->reserved_zero_mask);
    hash_u16(ctx, space->entry_count);
    for (size_t i = 0; i < space->entry_count; i++) {
        hash_id(ctx, space->entries[i].stable_id);
        hash_u64(ctx, space->entries[i].encoding);
    }
}

static bool record_trailing_fields_zero(const XrRuntimeRecordAbi *record) {
    for (size_t i = record->field_count; i < XR_RUNTIME_ABI_MAX_RECORD_FIELDS; i++) {
        if (!field_is_zero(&record->fields[i]))
            return false;
    }
    for (size_t i = record->namespace_count; i < XR_RUNTIME_ABI_MAX_ENUM_NAMESPACES; i++) {
        if (!namespace_is_zero(&record->namespaces[i]))
            return false;
    }
    return true;
}

static uint16_t namespace_field_role(uint16_t namespace_role) {
    switch ((XrRuntimeEnumNamespaceRole) namespace_role) {
        case XR_RUNTIME_NAMESPACE_SEMANTIC_DOMAIN:
            return XR_RUNTIME_FIELD_DOMAIN_SEMANTIC;
        case XR_RUNTIME_NAMESPACE_MATERIALIZATION:
            return XR_RUNTIME_FIELD_DOMAIN_MATERIALIZATION;
        case XR_RUNTIME_NAMESPACE_EXTENT_KIND:
            return XR_RUNTIME_FIELD_EXTENT_KIND;
        case XR_RUNTIME_NAMESPACE_EXTENT_OPERAND:
            return XR_RUNTIME_FIELD_EXTENT_OPERAND_INDEX;
        case XR_RUNTIME_NAMESPACE_LAYOUT_FLAGS:
            return XR_RUNTIME_FIELD_LAYOUT_FLAGS;
        default:
            return XR_RUNTIME_FIELD_ROLE_INVALID;
    }
}

static const XrRuntimePhysicalFieldAbi *find_field(const XrRuntimeRecordAbi *record,
                                                   uint16_t role) {
    for (size_t i = 0; i < record->field_count; i++) {
        if (record->fields[i].role == role)
            return &record->fields[i];
    }
    return NULL;
}

static uint8_t role_expected_encoding(uint16_t role) {
    switch ((XrRuntimeAbiFieldRole) role) {
        case XR_RUNTIME_FIELD_DOMAIN_CONTRACT_ID:
        case XR_RUNTIME_FIELD_EXTENT_ID:
        case XR_RUNTIME_FIELD_EXTENT_LAYOUT_ID:
        case XR_RUNTIME_FIELD_EXTENT_GROUP_ID:
        case XR_RUNTIME_FIELD_EXTENT_PROVIDER_ID:
        case XR_RUNTIME_FIELD_LAYOUT_DESCRIPTOR_ID:
        case XR_RUNTIME_FIELD_LAYOUT_ID:
        case XR_RUNTIME_FIELD_LAYOUT_OBJECT_KIND_ID:
        case XR_RUNTIME_FIELD_LAYOUT_EXTENT_ID:
        case XR_RUNTIME_FIELD_LAYOUT_ROOT_PLAN_ID:
        case XR_RUNTIME_FIELD_LAYOUT_DESTRUCTOR_ID:
        case XR_RUNTIME_FIELD_LAYOUT_CLONE_ID:
        case XR_RUNTIME_FIELD_LAYOUT_EQ_HASH_ID:
        case XR_RUNTIME_FIELD_EVALUATED_EXTENT_ID:
        case XR_RUNTIME_FIELD_GROUP_ID:
            return XR_RUNTIME_FIELD_STABLE_ID;
        case XR_RUNTIME_FIELD_EXTENT_FINGERPRINT:
        case XR_RUNTIME_FIELD_LAYOUT_EXTENT_FINGERPRINT:
        case XR_RUNTIME_FIELD_LAYOUT_FINGERPRINT:
        case XR_RUNTIME_FIELD_EVALUATED_EXTENT_FINGERPRINT:
        case XR_RUNTIME_FIELD_GROUP_FINGERPRINT:
            return XR_RUNTIME_FIELD_FINGERPRINT;
        case XR_RUNTIME_FIELD_LAYOUT_SEMANTIC_DOMAINS:
        case XR_RUNTIME_FIELD_LAYOUT_MATERIALIZATIONS:
        case XR_RUNTIME_FIELD_LAYOUT_FLAGS:
            return XR_RUNTIME_FIELD_BITSET;
        default:
            return XR_RUNTIME_FIELD_UNSIGNED;
    }
}

static uint16_t role_expected_width(uint16_t role) {
    switch ((XrRuntimeAbiFieldRole) role) {
        case XR_RUNTIME_FIELD_DOMAIN_CONTRACT_ID:
        case XR_RUNTIME_FIELD_EXTENT_ID:
        case XR_RUNTIME_FIELD_EXTENT_LAYOUT_ID:
        case XR_RUNTIME_FIELD_EXTENT_GROUP_ID:
        case XR_RUNTIME_FIELD_EXTENT_PROVIDER_ID:
        case XR_RUNTIME_FIELD_LAYOUT_DESCRIPTOR_ID:
        case XR_RUNTIME_FIELD_LAYOUT_ID:
        case XR_RUNTIME_FIELD_LAYOUT_OBJECT_KIND_ID:
        case XR_RUNTIME_FIELD_LAYOUT_EXTENT_ID:
        case XR_RUNTIME_FIELD_LAYOUT_ROOT_PLAN_ID:
        case XR_RUNTIME_FIELD_LAYOUT_DESTRUCTOR_ID:
        case XR_RUNTIME_FIELD_LAYOUT_CLONE_ID:
        case XR_RUNTIME_FIELD_LAYOUT_EQ_HASH_ID:
        case XR_RUNTIME_FIELD_EVALUATED_EXTENT_ID:
        case XR_RUNTIME_FIELD_GROUP_ID:
            return XR_STABLE_ID_BYTES;
        case XR_RUNTIME_FIELD_EXTENT_FINGERPRINT:
        case XR_RUNTIME_FIELD_LAYOUT_EXTENT_FINGERPRINT:
        case XR_RUNTIME_FIELD_LAYOUT_FINGERPRINT:
        case XR_RUNTIME_FIELD_EVALUATED_EXTENT_FINGERPRINT:
        case XR_RUNTIME_FIELD_GROUP_FINGERPRINT:
            return XR_FINGERPRINT_BYTES;
        case XR_RUNTIME_FIELD_DOMAIN_SEMANTIC:
        case XR_RUNTIME_FIELD_DOMAIN_MATERIALIZATION:
        case XR_RUNTIME_FIELD_EXTENT_KIND:
            return 1;
        case XR_RUNTIME_FIELD_EXTENT_OPERAND_INDEX:
        case XR_RUNTIME_FIELD_EXTENT_PART_INDEX:
        case XR_RUNTIME_FIELD_EXTENT_PART_COUNT:
        case XR_RUNTIME_FIELD_EVALUATED_PART_INDEX:
        case XR_RUNTIME_FIELD_EVALUATED_PART_COUNT:
        case XR_RUNTIME_FIELD_GROUP_PART_COUNT:
            return 2;
        case XR_RUNTIME_FIELD_EXTENT_TAIL_OFFSET:
        case XR_RUNTIME_FIELD_EXTENT_STRIDE:
        case XR_RUNTIME_FIELD_LAYOUT_FIXED_PREFIX_SIZE:
        case XR_RUNTIME_FIELD_LIMIT_MAX_ALLOCATION_BYTES:
        case XR_RUNTIME_FIELD_EVALUATED_BYTES:
        case XR_RUNTIME_FIELD_EVALUATED_OPERAND:
            return 8;
        default:
            return 4;
    }
}

static XrRuntimeAbiStatus verify_record_field_policies(const XrRuntimeRecordAbi *record) {
    for (size_t i = 0; i < record->field_count; i++) {
        const XrRuntimePhysicalFieldAbi *field = &record->fields[i];
        if (field->encoding != role_expected_encoding(field->role) ||
            field->width != role_expected_width(field->role) ||
            field->atomicity != XR_RUNTIME_FIELD_PLAIN ||
            field->index_semantics != XR_RUNTIME_INDEX_NONE)
            return XR_RUNTIME_ABI_INVALID_POLICY;
    }
    return XR_RUNTIME_ABI_OK;
}

static XrRuntimeAbiStatus verify_record(
    const XrRuntimeRecordAbi *record, uint16_t expected_kind,
    const uint16_t *expected_fields, size_t expected_field_count,
    const uint16_t *expected_namespaces, size_t expected_namespace_count) {
    if (record->field_count > XR_RUNTIME_ABI_MAX_RECORD_FIELDS ||
        record->namespace_count > XR_RUNTIME_ABI_MAX_ENUM_NAMESPACES)
        return XR_RUNTIME_ABI_BUDGET_EXCEEDED;
    if (record->schema_version != XR_RUNTIME_ABI_SCHEMA_VERSION)
        return XR_RUNTIME_ABI_INVALID_SCHEMA;
    if (record->record_kind != expected_kind ||
        record->namespace_count != expected_namespace_count || record->reserved16 != 0 ||
        record->reserved32 != 0 || record->reserved[0] != 0 || record->reserved[1] != 0)
        return XR_RUNTIME_ABI_INVALID_SHAPE;
    XrRuntimeAbiStatus status = verify_field_sequence(
        record->fields, record->field_count, record->size, record->alignment,
        expected_fields, expected_field_count);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    status = verify_record_field_policies(record);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    for (size_t i = 0; i < expected_namespace_count; i++) {
        status = verify_namespace(&record->namespaces[i], expected_namespaces[i]);
        if (status != XR_RUNTIME_ABI_OK)
            return status;
        uint16_t role = namespace_field_role(expected_namespaces[i]);
        const XrRuntimePhysicalFieldAbi *field = find_field(record, role);
        if (!field || field->width != record->namespaces[i].encoding_width)
            return XR_RUNTIME_ABI_INVALID_SHAPE;
    }
    return record_trailing_fields_zero(record) ? XR_RUNTIME_ABI_OK
                                                : XR_RUNTIME_ABI_INVALID_POLICY;
}

static void hash_record(XrSHA256Context *ctx, const XrRuntimeRecordAbi *record) {
    hash_u32(ctx, record->schema_version);
    hash_u16(ctx, record->record_kind);
    hash_u16(ctx, record->size);
    hash_u16(ctx, record->alignment);
    hash_u16(ctx, record->field_count);
    for (size_t i = 0; i < record->field_count; i++)
        hash_field(ctx, &record->fields[i]);
    hash_u16(ctx, record->namespace_count);
    for (size_t i = 0; i < record->namespace_count; i++)
        hash_namespace(ctx, &record->namespaces[i]);
}

static const uint16_t domain_fields[] = {
    XR_RUNTIME_FIELD_DOMAIN_CONTRACT_ID,
    XR_RUNTIME_FIELD_DOMAIN_INSTANCE_ID,
    XR_RUNTIME_FIELD_DOMAIN_SEMANTIC,
    XR_RUNTIME_FIELD_DOMAIN_MATERIALIZATION,
};
static const uint16_t domain_namespaces[] = {
    XR_RUNTIME_NAMESPACE_SEMANTIC_DOMAIN,
    XR_RUNTIME_NAMESPACE_MATERIALIZATION,
};
static const uint16_t extent_fields[] = {
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
};
static const uint16_t extent_namespaces[] = {
    XR_RUNTIME_NAMESPACE_EXTENT_KIND,
    XR_RUNTIME_NAMESPACE_EXTENT_OPERAND,
};
static const uint16_t layout_fields[] = {
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
};
static const uint16_t layout_namespaces[] = {XR_RUNTIME_NAMESPACE_LAYOUT_FLAGS};
static const uint16_t limits_fields[] = {
    XR_RUNTIME_FIELD_LIMIT_MAX_ALLOCATION_BYTES,
    XR_RUNTIME_FIELD_LIMIT_MAX_ALIGNMENT,
};
static const uint16_t evaluated_fields[] = {
    XR_RUNTIME_FIELD_EVALUATED_EXTENT_ID,
    XR_RUNTIME_FIELD_EVALUATED_EXTENT_FINGERPRINT,
    XR_RUNTIME_FIELD_EVALUATED_BYTES,
    XR_RUNTIME_FIELD_EVALUATED_OPERAND,
    XR_RUNTIME_FIELD_EVALUATED_ALIGNMENT,
    XR_RUNTIME_FIELD_EVALUATED_PART_INDEX,
    XR_RUNTIME_FIELD_EVALUATED_PART_COUNT,
};
static const uint16_t group_fields[] = {
    XR_RUNTIME_FIELD_GROUP_ID,
    XR_RUNTIME_FIELD_GROUP_PART_COUNT,
    XR_RUNTIME_FIELD_GROUP_FINGERPRINT,
};

static XrRuntimeAbiStatus verify_runtime_records(const XrRuntimeAbiContract *abi) {
    XrRuntimeAbiStatus status = verify_record(
        &abi->domain_identity, XR_RUNTIME_RECORD_DOMAIN_IDENTITY, domain_fields,
        sizeof(domain_fields) / sizeof(domain_fields[0]), domain_namespaces,
        sizeof(domain_namespaces) / sizeof(domain_namespaces[0]));
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    status = verify_record(&abi->extent_descriptor, XR_RUNTIME_RECORD_EXTENT_DESCRIPTOR,
                           extent_fields, sizeof(extent_fields) / sizeof(extent_fields[0]),
                           extent_namespaces,
                           sizeof(extent_namespaces) / sizeof(extent_namespaces[0]));
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    status = verify_record(&abi->layout_descriptor, XR_RUNTIME_RECORD_LAYOUT_DESCRIPTOR,
                           layout_fields, sizeof(layout_fields) / sizeof(layout_fields[0]),
                           layout_namespaces,
                           sizeof(layout_namespaces) / sizeof(layout_namespaces[0]));
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    status = verify_record(&abi->extent_limits, XR_RUNTIME_RECORD_EXTENT_LIMITS,
                           limits_fields, sizeof(limits_fields) / sizeof(limits_fields[0]),
                           NULL, 0);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    status = verify_record(&abi->evaluated_extent, XR_RUNTIME_RECORD_EVALUATED_EXTENT,
                           evaluated_fields,
                           sizeof(evaluated_fields) / sizeof(evaluated_fields[0]), NULL, 0);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    return verify_record(&abi->extent_group_summary,
                         XR_RUNTIME_RECORD_EXTENT_GROUP_SUMMARY, group_fields,
                         sizeof(group_fields) / sizeof(group_fields[0]), NULL, 0);
}

static XrRuntimeAbiStatus verify_status_namespace(
    const XrRuntimeEnumNamespaceAbi *space) {
    XrRuntimeAbiStatus status =
        verify_namespace(space, XR_RUNTIME_NAMESPACE_STATUS);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    if (space->kind != XR_RUNTIME_NAMESPACE_ENUM ||
        space->entry_count != XR_RUNTIME_ABI_STATUS_COUNT)
        return XR_RUNTIME_ABI_INVALID_SHAPE;
    for (uint64_t value = 0; value < XR_RUNTIME_ABI_STATUS_COUNT; value++) {
        bool found = false;
        for (size_t i = 0; i < space->entry_count; i++) {
            if (space->entries[i].encoding == value) {
                found = true;
                break;
            }
        }
        if (!found)
            return XR_RUNTIME_ABI_INVALID_SHAPE;
    }
    return XR_RUNTIME_ABI_OK;
}

static XrRuntimeAbiStatus verify_extent_provider_callback(
    const XrRuntimeExtentProviderCallbackAbi *callback, uint16_t pointer_width) {
    if (callback->schema_version != XR_RUNTIME_ABI_SCHEMA_VERSION)
        return XR_RUNTIME_ABI_INVALID_SCHEMA;
    if (id_is_zero(callback->contract_id) ||
        callback->provider_id_width != XR_STABLE_ID_BYTES ||
        callback->operand_element_width != sizeof(uint64_t) ||
        callback->operand_count_width != pointer_width ||
        callback->result_width != sizeof(uint64_t))
        return XR_RUNTIME_ABI_INVALID_SHAPE;
    if (callback->error_normalization != XR_RUNTIME_PROVIDER_ERROR_NON_OK_TO_REJECTED ||
        callback->reserved8 != 0 || callback->reserved16 != 0 ||
        callback->reserved32 != 0 || callback->reserved[0] != 0 ||
        callback->reserved[1] != 0)
        return XR_RUNTIME_ABI_INVALID_POLICY;
    return verify_status_namespace(&callback->status_namespace);
}

static XrRuntimeAbiStatus verify_runtime_contract(const XrRuntimeAbiContract *abi,
                                                  XrFingerprint *header_fingerprint) {
    if (!abi)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    if (abi->schema_version != XR_RUNTIME_ABI_SCHEMA_VERSION)
        return XR_RUNTIME_ABI_INVALID_SCHEMA;
    if (abi->stable_id_width != XR_STABLE_ID_BYTES ||
        abi->fingerprint_width != XR_FINGERPRINT_BYTES ||
        (abi->pointer_width != 4 && abi->pointer_width != 8))
        return XR_RUNTIME_ABI_INVALID_SHAPE;
    if (abi->canonical_serialization_endian != XR_RUNTIME_ENDIAN_LITTLE ||
        (abi->target_endian != XR_RUNTIME_ENDIAN_LITTLE &&
         abi->target_endian != XR_RUNTIME_ENDIAN_BIG) ||
        abi->checked_arithmetic_policy != XR_RUNTIME_CHECKED_ARITHMETIC_REJECT_OVERFLOW ||
        abi->alignment_policy != XR_RUNTIME_ALIGNMENT_POWER_OF_TWO_REJECT_OVERFLOW ||
        abi->unknown_enum_policy != XR_RUNTIME_UNKNOWN_ENUM_REJECT ||
        abi->reserved_zero_policy != XR_RUNTIME_RESERVED_ZERO_REJECT ||
        abi->reserved16 != 0 || abi->reserved32 != 0 || abi->reserved[0] != 0 ||
        abi->reserved[1] != 0)
        return XR_RUNTIME_ABI_INVALID_POLICY;

    XrRuntimeAbiStatus status =
        xr_runtime_object_header_abi_fingerprint(&abi->object_header, header_fingerprint);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    if (abi->object_header.target_endian != abi->target_endian)
        return XR_RUNTIME_ABI_INVALID_POLICY;
    status = verify_dynamic_value(&abi->dynamic_value, abi->pointer_width,
                                  abi->target_endian);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    status = verify_runtime_records(abi);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    return verify_extent_provider_callback(&abi->extent_provider_callback,
                                           abi->pointer_width);
}

static void hash_runtime_contract(XrSHA256Context *ctx, const XrRuntimeAbiContract *abi,
                                  XrFingerprint header_fingerprint) {
    hash_u32(ctx, abi->schema_version);
    hash_u16(ctx, abi->stable_id_width);
    hash_u16(ctx, abi->fingerprint_width);
    hash_u16(ctx, abi->pointer_width);
    hash_u8(ctx, abi->canonical_serialization_endian);
    hash_u8(ctx, abi->target_endian);
    hash_u8(ctx, abi->checked_arithmetic_policy);
    hash_u8(ctx, abi->alignment_policy);
    hash_u8(ctx, abi->unknown_enum_policy);
    hash_u8(ctx, abi->reserved_zero_policy);
    hash_fingerprint(ctx, header_fingerprint);
    hash_dynamic_value(ctx, &abi->dynamic_value);
    hash_record(ctx, &abi->domain_identity);
    hash_record(ctx, &abi->extent_descriptor);
    hash_record(ctx, &abi->layout_descriptor);
    hash_record(ctx, &abi->extent_limits);
    hash_record(ctx, &abi->evaluated_extent);
    hash_record(ctx, &abi->extent_group_summary);
    const XrRuntimeExtentProviderCallbackAbi *callback = &abi->extent_provider_callback;
    hash_u32(ctx, callback->schema_version);
    hash_id(ctx, callback->contract_id);
    hash_u16(ctx, callback->provider_id_width);
    hash_u16(ctx, callback->operand_element_width);
    hash_u16(ctx, callback->operand_count_width);
    hash_u16(ctx, callback->result_width);
    hash_u8(ctx, callback->error_normalization);
    hash_namespace(ctx, &callback->status_namespace);
}

XrRuntimeAbiStatus xr_runtime_abi_contract_fingerprint(
    const XrRuntimeAbiContract *abi, XrFingerprint *out) {
    if (!out)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    XrFingerprint header_fingerprint;
    XrRuntimeAbiStatus status = verify_runtime_contract(abi, &header_fingerprint);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    XrSHA256Context ctx;
    hash_begin(&ctx, XR_RUNTIME_CONTRACT_WHOLE_RUNTIME);
    hash_runtime_contract(&ctx, abi, header_fingerprint);
    XrFingerprint fingerprint;
    xr_sha256_final(&ctx, fingerprint.bytes);
    *out = fingerprint;
    return XR_RUNTIME_ABI_OK;
}

static bool provider_call_slot_is_zero(const XrTargetProviderCallSlotAbi *slot) {
    return slot->value_kind == 0 && slot->width == 0 && slot->alignment == 0 &&
           slot->ownership == 0 && slot->flags == 0 &&
           bytes_are_zero(slot->reserved8, sizeof(slot->reserved8)) &&
           slot->reserved64 == 0;
}

static bool provider_call_abi_is_zero(const XrTargetProviderCallAbiContract *abi) {
    if (abi->schema_version != 0 || abi->parameter_count != 0 ||
        abi->calling_convention != 0 || abi->target_endian != 0 ||
        abi->pointer_width != 0 || abi->pointer_alignment != 0 || abi->variadic != 0 ||
        abi->reserved8 != 0 || abi->reserved32 != 0 ||
        !provider_call_slot_is_zero(&abi->result) || abi->reserved[0] != 0 ||
        abi->reserved[1] != 0)
        return false;
    for (size_t i = 0; i < XR_TARGET_PROVIDER_CALL_ABI_MAX_PARAMETERS; i++) {
        if (!provider_call_slot_is_zero(&abi->parameters[i]))
            return false;
    }
    return true;
}

static XrRuntimeAbiStatus verify_provider_call_slot(
    const XrTargetProviderCallAbiContract *abi,
    const XrTargetProviderCallSlotAbi *slot, bool is_result,
    uint32_t *derived_lifetime_flags) {
    if (!bytes_are_zero(slot->reserved8, sizeof(slot->reserved8)) ||
        slot->reserved64 != 0)
        return XR_RUNTIME_ABI_INVALID_POLICY;
    if ((slot->flags & ~XR_TARGET_PROVIDER_CALL_SLOT_FLAGS_ALL) != 0)
        return XR_RUNTIME_ABI_INVALID_MASK;

    switch ((XrTargetProviderCallValueKind) slot->value_kind) {
        case XR_TARGET_PROVIDER_CALL_VALUE_VOID:
            if (!is_result || slot->width != 0 || slot->alignment != 0 ||
                slot->ownership != XR_TARGET_PROVIDER_CALL_OWNERSHIP_NONE ||
                slot->flags != 0)
                return XR_RUNTIME_ABI_INVALID_SHAPE;
            return XR_RUNTIME_ABI_OK;
        case XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER:
        case XR_TARGET_PROVIDER_CALL_VALUE_UNSIGNED_INTEGER:
        case XR_TARGET_PROVIDER_CALL_VALUE_IEEE_FLOAT:
            if (!scalar_width_valid(slot->width) ||
                !is_power_of_two_u64(slot->alignment) ||
                slot->alignment > slot->width ||
                slot->ownership != XR_TARGET_PROVIDER_CALL_OWNERSHIP_NONE ||
                slot->flags != 0 ||
                (slot->value_kind == XR_TARGET_PROVIDER_CALL_VALUE_IEEE_FLOAT &&
                 slot->width != 4 && slot->width != 8))
                return XR_RUNTIME_ABI_INVALID_SHAPE;
            return XR_RUNTIME_ABI_OK;
        case XR_TARGET_PROVIDER_CALL_VALUE_DATA_ADDRESS:
        case XR_TARGET_PROVIDER_CALL_VALUE_CODE_ADDRESS:
            if (slot->width != abi->pointer_width ||
                slot->alignment != abi->pointer_alignment ||
                (slot->value_kind == XR_TARGET_PROVIDER_CALL_VALUE_CODE_ADDRESS &&
                 (slot->flags & XR_TARGET_PROVIDER_CALL_SLOT_CONST_POINTEE) != 0))
                return XR_RUNTIME_ABI_INVALID_SHAPE;
            if (is_result) {
                if (slot->ownership != XR_TARGET_PROVIDER_CALL_OWNERSHIP_NONE &&
                    !(slot->value_kind == XR_TARGET_PROVIDER_CALL_VALUE_DATA_ADDRESS &&
                      slot->ownership ==
                          XR_TARGET_PROVIDER_CALL_OWNERSHIP_RETURNED_OWNED))
                    return XR_RUNTIME_ABI_INVALID_SHAPE;
                if (slot->ownership == XR_TARGET_PROVIDER_CALL_OWNERSHIP_RETURNED_OWNED)
                    *derived_lifetime_flags |=
                        XR_TARGET_PROVIDER_LIFETIME_RETURNS_OWNED;
            } else {
                if (slot->ownership != XR_TARGET_PROVIDER_CALL_OWNERSHIP_NONE &&
                    slot->ownership != XR_TARGET_PROVIDER_CALL_OWNERSHIP_BORROWED &&
                    !(slot->value_kind == XR_TARGET_PROVIDER_CALL_VALUE_DATA_ADDRESS &&
                      slot->ownership == XR_TARGET_PROVIDER_CALL_OWNERSHIP_CONSUMED))
                    return XR_RUNTIME_ABI_INVALID_SHAPE;
                if (slot->ownership == XR_TARGET_PROVIDER_CALL_OWNERSHIP_BORROWED)
                    *derived_lifetime_flags |= XR_TARGET_PROVIDER_LIFETIME_BORROWS;
                if (slot->ownership == XR_TARGET_PROVIDER_CALL_OWNERSHIP_CONSUMED)
                    *derived_lifetime_flags |=
                        XR_TARGET_PROVIDER_LIFETIME_CONSUMES_OWNED;
                if (slot->value_kind == XR_TARGET_PROVIDER_CALL_VALUE_CODE_ADDRESS)
                    *derived_lifetime_flags |= XR_TARGET_PROVIDER_LIFETIME_CALLBACK;
            }
            return XR_RUNTIME_ABI_OK;
        default:
            return XR_RUNTIME_ABI_INVALID_SHAPE;
    }
}

static XrRuntimeAbiStatus verify_provider_call_abi(
    const XrTargetProviderCallAbiContract *abi, uint32_t *derived_lifetime_flags) {
    if (!abi)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    if (abi->schema_version != XR_RUNTIME_ABI_SCHEMA_VERSION)
        return XR_RUNTIME_ABI_INVALID_SCHEMA;
    if (abi->parameter_count > XR_TARGET_PROVIDER_CALL_ABI_MAX_PARAMETERS)
        return XR_RUNTIME_ABI_BUDGET_EXCEEDED;
    if (abi->calling_convention != XR_TARGET_PROVIDER_CALLING_CONVENTION_C ||
        (abi->target_endian != XR_RUNTIME_ENDIAN_LITTLE &&
         abi->target_endian != XR_RUNTIME_ENDIAN_BIG) ||
        (abi->pointer_width != 4 && abi->pointer_width != 8) ||
        !is_power_of_two_u64(abi->pointer_alignment) ||
        abi->pointer_alignment > abi->pointer_width || abi->variadic != 0)
        return XR_RUNTIME_ABI_INVALID_SHAPE;
    if (abi->reserved8 != 0 || abi->reserved32 != 0 || abi->reserved[0] != 0 ||
        abi->reserved[1] != 0)
        return XR_RUNTIME_ABI_INVALID_POLICY;

    uint32_t lifetime_flags = 0;
    XrRuntimeAbiStatus status =
        verify_provider_call_slot(abi, &abi->result, true, &lifetime_flags);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    for (size_t i = 0; i < abi->parameter_count; i++) {
        status = verify_provider_call_slot(abi, &abi->parameters[i], false,
                                           &lifetime_flags);
        if (status != XR_RUNTIME_ABI_OK)
            return status;
    }
    for (size_t i = abi->parameter_count;
         i < XR_TARGET_PROVIDER_CALL_ABI_MAX_PARAMETERS; i++) {
        if (!provider_call_slot_is_zero(&abi->parameters[i]))
            return XR_RUNTIME_ABI_INVALID_POLICY;
    }
    if (derived_lifetime_flags)
        *derived_lifetime_flags = lifetime_flags;
    return XR_RUNTIME_ABI_OK;
}

static void hash_provider_call_slot(XrSHA256Context *ctx,
                                    const XrTargetProviderCallSlotAbi *slot) {
    hash_u8(ctx, slot->value_kind);
    hash_u8(ctx, slot->width);
    hash_u8(ctx, slot->alignment);
    hash_u8(ctx, slot->ownership);
    hash_u8(ctx, slot->flags);
}

static void hash_provider_call_abi(
    XrSHA256Context *ctx, const XrTargetProviderCallAbiContract *abi) {
    hash_u32(ctx, abi->schema_version);
    hash_u8(ctx, abi->calling_convention);
    hash_u8(ctx, abi->target_endian);
    hash_u8(ctx, abi->pointer_width);
    hash_u8(ctx, abi->pointer_alignment);
    hash_u8(ctx, abi->variadic);
    hash_provider_call_slot(ctx, &abi->result);
    hash_u16(ctx, abi->parameter_count);
    for (size_t i = 0; i < abi->parameter_count; i++)
        hash_provider_call_slot(ctx, &abi->parameters[i]);
}

XrRuntimeAbiStatus xr_target_provider_call_abi_fingerprint(
    const XrTargetProviderCallAbiContract *abi, XrFingerprint *out) {
    if (!out)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    XrRuntimeAbiStatus status = verify_provider_call_abi(abi, NULL);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    XrSHA256Context ctx;
    hash_begin(&ctx, XR_RUNTIME_CONTRACT_PROVIDER_CALL_ABI);
    hash_provider_call_abi(&ctx, abi);
    XrFingerprint fingerprint;
    xr_sha256_final(&ctx, fingerprint.bytes);
    *out = fingerprint;
    return XR_RUNTIME_ABI_OK;
}

static bool provider_operation_is_zero(
    const XrTargetProviderOperationContract *operation) {
    return id_is_zero(operation->stable_id) &&
           provider_call_abi_is_zero(&operation->call_abi) &&
           operation->effect_flags == 0 && operation->lifetime_flags == 0 &&
           operation->failure_flags == 0 && operation->reserved32 == 0 &&
           operation->reserved64 == 0;
}

static XrRuntimeAbiStatus verify_provider_operations(
    const XrTargetProviderContract *provider, uint32_t *out_effects,
    uint32_t *out_failures) {
    if (provider->operation_count > XR_RUNTIME_ABI_MAX_PROVIDER_OPERATIONS)
        return XR_RUNTIME_ABI_BUDGET_EXCEEDED;
    if (provider->operation_count == 0)
        return XR_RUNTIME_ABI_INVALID_PROVIDER_SET;
    uint32_t effects = 0;
    uint32_t failures = 0;
    for (size_t i = 0; i < provider->operation_count; i++) {
        const XrTargetProviderOperationContract *operation = &provider->operations[i];
        uint32_t call_lifetime_flags = 0;
        XrRuntimeAbiStatus call_status =
            verify_provider_call_abi(&operation->call_abi, &call_lifetime_flags);
        if (call_status != XR_RUNTIME_ABI_OK)
            return call_status;
        if (id_is_zero(operation->stable_id) || operation->effect_flags == 0 ||
            (operation->effect_flags & ~XR_TARGET_PROVIDER_EFFECT_FLAGS_ALL) != 0 ||
            (operation->lifetime_flags & ~XR_TARGET_PROVIDER_LIFETIME_FLAGS_ALL) != 0 ||
            (operation->failure_flags & ~XR_TARGET_PROVIDER_FAILURE_FLAGS_ALL) != 0 ||
            operation->lifetime_flags != call_lifetime_flags ||
            operation->reserved32 != 0 || operation->reserved64 != 0)
            return XR_RUNTIME_ABI_INVALID_PROVIDER_SET;
        if (i != 0 && id_compare(provider->operations[i - 1].stable_id,
                                 operation->stable_id) >= 0)
            return XR_RUNTIME_ABI_INVALID_ORDER;
        effects |= operation->effect_flags;
        failures |= operation->failure_flags;
    }
    for (size_t i = provider->operation_count;
         i < XR_RUNTIME_ABI_MAX_PROVIDER_OPERATIONS; i++) {
        if (!provider_operation_is_zero(&provider->operations[i]))
            return XR_RUNTIME_ABI_INVALID_POLICY;
    }
    *out_effects = effects;
    *out_failures = failures;
    return XR_RUNTIME_ABI_OK;
}

static bool provider_available_for_profile(const XrTargetProviderContract *provider) {
    uint32_t required_flag =
        provider->runtime_profile == XR_TARGET_RUNTIME_PROFILE_HOSTED
            ? XR_TARGET_PROVIDER_AVAILABLE_HOSTED
            : XR_TARGET_PROVIDER_AVAILABLE_FREESTANDING;
    return (provider->flags & required_flag) != 0;
}

static XrRuntimeAbiStatus verify_provider_kind_facts(
    const XrTargetProviderContract *provider, uint32_t effects, uint32_t failures) {
    bool allocator_booleans_valid = provider->allocator_sized_free <= 1 &&
                                    provider->allocator_zeroed_allocation <= 1 &&
                                    provider->allocator_thread_safe <= 1;
    if (!allocator_booleans_valid)
        return XR_RUNTIME_ABI_INVALID_PROVIDER_SET;
    switch ((XrTargetProviderKind) provider->provider_kind) {
        case XR_TARGET_PROVIDER_ALLOCATOR:
            if (!is_power_of_two_u64(provider->allocator_max_alignment) ||
                provider->panic_behavior != XR_TARGET_PROVIDER_PANIC_INVALID ||
                (effects & XR_TARGET_PROVIDER_EFFECT_ALLOCATES) == 0 ||
                (effects & XR_TARGET_PROVIDER_EFFECT_DEALLOCATES) == 0)
                return XR_RUNTIME_ABI_INVALID_PROVIDER_SET;
            break;
        case XR_TARGET_PROVIDER_PANIC:
            if (provider->allocator_max_alignment != 0 || provider->allocator_sized_free != 0 ||
                provider->allocator_zeroed_allocation != 0 ||
                provider->allocator_thread_safe != 0 ||
                (provider->panic_behavior != XR_TARGET_PROVIDER_PANIC_NO_RETURN &&
                 provider->panic_behavior != XR_TARGET_PROVIDER_PANIC_UNWINDS) ||
                (effects & XR_TARGET_PROVIDER_EFFECT_PANICS) == 0)
                return XR_RUNTIME_ABI_INVALID_PROVIDER_SET;
            if (provider->panic_behavior == XR_TARGET_PROVIDER_PANIC_NO_RETURN &&
                (failures & XR_TARGET_PROVIDER_FAILURE_NO_RETURN) == 0)
                return XR_RUNTIME_ABI_INVALID_PROVIDER_SET;
            if (provider->panic_behavior == XR_TARGET_PROVIDER_PANIC_UNWINDS &&
                (failures & XR_TARGET_PROVIDER_FAILURE_PANICS) == 0)
                return XR_RUNTIME_ABI_INVALID_PROVIDER_SET;
            break;
        default:
            if (provider->allocator_max_alignment != 0 || provider->allocator_sized_free != 0 ||
                provider->allocator_zeroed_allocation != 0 ||
                provider->allocator_thread_safe != 0 ||
                provider->panic_behavior != XR_TARGET_PROVIDER_PANIC_INVALID)
                return XR_RUNTIME_ABI_INVALID_PROVIDER_SET;
            break;
    }
    return XR_RUNTIME_ABI_OK;
}

static XrRuntimeAbiStatus verify_provider(
    const XrTargetProviderContract *provider, uint8_t expected_profile) {
    if (provider->schema_version != XR_RUNTIME_ABI_SCHEMA_VERSION ||
        provider->abi_schema_version != XR_RUNTIME_ABI_SCHEMA_VERSION)
        return XR_RUNTIME_ABI_INVALID_SCHEMA;
    if (id_is_zero(provider->contract_id) || provider->runtime_profile != expected_profile ||
        provider->provider_kind <= XR_TARGET_PROVIDER_INVALID ||
        provider->provider_kind >= XR_TARGET_PROVIDER_KIND_COUNT || provider->flags == 0 ||
        (provider->flags & ~XR_TARGET_PROVIDER_FLAGS_ALL) != 0 ||
        !provider_available_for_profile(provider))
        return XR_RUNTIME_ABI_INVALID_PROVIDER_SET;
    if (provider->reserved32 != 0 || provider->reserved[0] != 0 ||
        provider->reserved[1] != 0)
        return XR_RUNTIME_ABI_INVALID_POLICY;
    uint32_t effects = 0;
    uint32_t failures = 0;
    XrRuntimeAbiStatus status =
        verify_provider_operations(provider, &effects, &failures);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    return verify_provider_kind_facts(provider, effects, failures);
}

static XrRuntimeAbiStatus verify_provider_set(
    const XrTargetProviderContract *providers, size_t provider_count,
    uint64_t *derived_mask) {
    if (!providers)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    if (provider_count > XR_RUNTIME_ABI_MAX_PROVIDERS)
        return XR_RUNTIME_ABI_BUDGET_EXCEEDED;
    if (provider_count == 0)
        return XR_RUNTIME_ABI_INVALID_PROVIDER_SET;
    uint8_t profile = providers[0].runtime_profile;
    if (profile != XR_TARGET_RUNTIME_PROFILE_HOSTED &&
        profile != XR_TARGET_RUNTIME_PROFILE_FREESTANDING)
        return XR_RUNTIME_ABI_INVALID_PROVIDER_SET;

    uint64_t mask = 0;
    for (size_t i = 0; i < provider_count; i++) {
        XrRuntimeAbiStatus status = verify_provider(&providers[i], profile);
        if (status != XR_RUNTIME_ABI_OK)
            return status;
        if (i != 0 && providers[i - 1].provider_kind >= providers[i].provider_kind)
            return XR_RUNTIME_ABI_INVALID_ORDER;
        for (size_t j = 0; j < i; j++) {
            if (id_compare(providers[j].contract_id, providers[i].contract_id) == 0)
                return XR_RUNTIME_ABI_INVALID_ORDER;
        }
        mask |= XR_TARGET_PROVIDER_MASK(providers[i].provider_kind);
    }
    uint64_t required = XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_ALLOCATOR) |
                        XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_PANIC);
    if ((mask & required) != required || (mask & ~XR_TARGET_PROVIDER_MASK_ALL) != 0)
        return XR_RUNTIME_ABI_INVALID_PROVIDER_SET;
    *derived_mask = mask;
    return XR_RUNTIME_ABI_OK;
}

static void hash_provider_operation(
    XrSHA256Context *ctx, const XrTargetProviderOperationContract *operation) {
    hash_id(ctx, operation->stable_id);
    hash_provider_call_abi(ctx, &operation->call_abi);
    hash_u32(ctx, operation->effect_flags);
    hash_u32(ctx, operation->lifetime_flags);
    hash_u32(ctx, operation->failure_flags);
}

static void hash_provider(XrSHA256Context *ctx,
                          const XrTargetProviderContract *provider) {
    hash_u32(ctx, provider->schema_version);
    hash_u8(ctx, provider->runtime_profile);
    hash_u8(ctx, provider->provider_kind);
    hash_id(ctx, provider->contract_id);
    hash_u32(ctx, provider->abi_schema_version);
    hash_u32(ctx, provider->flags);
    hash_u32(ctx, provider->allocator_max_alignment);
    hash_u8(ctx, provider->allocator_sized_free);
    hash_u8(ctx, provider->allocator_zeroed_allocation);
    hash_u8(ctx, provider->allocator_thread_safe);
    hash_u8(ctx, provider->panic_behavior);
    hash_u16(ctx, provider->operation_count);
    for (size_t i = 0; i < provider->operation_count; i++)
        hash_provider_operation(ctx, &provider->operations[i]);
}

XrRuntimeAbiStatus xr_target_provider_set_fingerprint(
    const XrTargetProviderContract *providers, size_t provider_count,
    uint64_t *out_provider_mask, XrFingerprint *out) {
    if (!out_provider_mask || !out)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    uint64_t provider_mask = 0;
    XrRuntimeAbiStatus status =
        verify_provider_set(providers, provider_count, &provider_mask);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    XrSHA256Context ctx;
    hash_begin(&ctx, XR_RUNTIME_CONTRACT_PROVIDER_SET);
    hash_u8(&ctx, providers[0].runtime_profile);
    hash_u16(&ctx, (uint16_t) provider_count);
    hash_u64(&ctx, provider_mask);
    for (size_t i = 0; i < provider_count; i++)
        hash_provider(&ctx, &providers[i]);
    XrFingerprint fingerprint;
    xr_sha256_final(&ctx, fingerprint.bytes);
    *out_provider_mask = provider_mask;
    *out = fingerprint;
    return XR_RUNTIME_ABI_OK;
}
