/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_runtime_object_header.c - Canonical object-header materialization
 */

#include "xr_runtime_object_header.h"
#include <string.h>

/* This table is the production identity/encoding source. Stable IDs are the
 * first 128 bits of SHA-256 over xray-entity-id-v1\0, a u32 little-endian key
 * length, and the canonical UTF-8 key named on each row. Rows are ordered by
 * stable-ID bytes; numeric encodings remain independent ABI facts. */
static const XrRuntimeObjectKindAbiEntry canonical_object_kinds[] = {
    /* xray.runtime.object-kind.v1/string */
    {{{0x01, 0xad, 0x29, 0x3f, 0x96, 0xed, 0xa5, 0xf4,
       0xa5, 0x5e, 0xec, 0x47, 0x56, 0x41, 0x15, 0x78}},
     XR_RUNTIME_OBJECT_KIND_STRING, 0},
    /* xray.runtime.object-kind.v1/closure */
    {{{0x44, 0xa9, 0x3f, 0xc9, 0x77, 0xde, 0x3a, 0x91,
       0xdf, 0x2b, 0xcd, 0xed, 0xf8, 0xe2, 0x4b, 0xbe}},
     XR_RUNTIME_OBJECT_KIND_CLOSURE, 0},
    /* xray.runtime.object-kind.v1/enum-box */
    {{{0x4e, 0xa8, 0x35, 0x4c, 0x96, 0xf7, 0x05, 0x3e,
       0x2c, 0xe9, 0xe3, 0x61, 0x91, 0xc0, 0x5f, 0xb8}},
     XR_RUNTIME_OBJECT_KIND_ENUM_BOX, 0},
    /* xray.runtime.object-kind.v1/cell */
    {{{0x50, 0x02, 0xb7, 0x72, 0xfa, 0xcb, 0xd3, 0x12,
       0xb4, 0xea, 0x6c, 0x70, 0x89, 0xb7, 0x53, 0x5b}},
     XR_RUNTIME_OBJECT_KIND_CELL, 0},
    /* xray.runtime.object-kind.v1/instance */
    {{{0x50, 0xb0, 0xeb, 0xb3, 0x15, 0x53, 0xf8, 0x79,
       0x76, 0xe3, 0x4c, 0x6d, 0x22, 0xbc, 0x5b, 0x6c}},
     XR_RUNTIME_OBJECT_KIND_INSTANCE, 0},
    /* xray.runtime.object-kind.v1/set */
    {{{0x62, 0x9a, 0x25, 0xba, 0x5b, 0x73, 0x39, 0xa4,
       0x3e, 0x86, 0xf2, 0x5a, 0x45, 0x4c, 0xa5, 0x0b}},
     XR_RUNTIME_OBJECT_KIND_SET, 0},
    /* xray.runtime.object-kind.v1/array */
    {{{0x6f, 0x16, 0xe8, 0x54, 0x15, 0x00, 0xc5, 0x6f,
       0xcd, 0x88, 0x01, 0xc2, 0xbd, 0x76, 0x36, 0xe2}},
     XR_RUNTIME_OBJECT_KIND_ARRAY, 0},
    /* xray.runtime.object-kind.v1/map */
    {{{0x9d, 0xc5, 0x4e, 0xba, 0x4e, 0x52, 0xd2, 0x31,
       0x04, 0xda, 0xed, 0xc4, 0xeb, 0xe5, 0xcd, 0xea}},
     XR_RUNTIME_OBJECT_KIND_MAP, 0},
    /* xray.runtime.object-kind.v1/boxed-aggregate */
    {{{0xe1, 0x32, 0x93, 0x52, 0x71, 0x9c, 0x2c, 0x5e,
       0x8b, 0x58, 0xcd, 0xc6, 0x8d, 0x36, 0xcd, 0x30}},
     XR_RUNTIME_OBJECT_KIND_BOXED_AGGREGATE, 0},
};

_Static_assert(sizeof(canonical_object_kinds) / sizeof(canonical_object_kinds[0]) ==
                   XR_RUNTIME_OBJECT_KIND_COUNT - 1,
               "canonical object-kind registry is incomplete");

static bool object_kind_valid(uint16_t object_kind) {
    return object_kind > XR_RUNTIME_OBJECT_KIND_INVALID &&
           object_kind < XR_RUNTIME_OBJECT_KIND_COUNT;
}

static bool facts_reserved_zero(
    const XrRuntimeObjectHeaderMaterializationFacts *facts) {
    return facts->reserved32 == 0 && facts->reserved[0] == 0 &&
           facts->reserved[1] == 0;
}

static XrRuntimeAbiStatus verify_facts(
    const XrRuntimeObjectHeaderMaterializationFacts *facts) {
    if (!facts)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    if (facts->schema_version != XR_RUNTIME_OBJECT_HEADER_FACTS_SCHEMA_VERSION)
        return XR_RUNTIME_ABI_INVALID_SCHEMA;
    if (facts->target_endian != XR_RUNTIME_ENDIAN_LITTLE &&
        facts->target_endian != XR_RUNTIME_ENDIAN_BIG)
        return XR_RUNTIME_ABI_INVALID_POLICY;
    if (!facts_reserved_zero(facts))
        return XR_RUNTIME_ABI_INVALID_POLICY;
    if (facts->header_size != XR_RUNTIME_OBJECT_HEADER_SIZE ||
        facts->header_alignment != XR_RUNTIME_OBJECT_HEADER_ALIGNMENT ||
        facts->atomic_i32_size != 4 || facts->atomic_i32_alignment != 4 ||
        facts->uint16_size != 2 || facts->uint16_alignment != 2 ||
        facts->uint32_size != 4 || facts->uint32_alignment != 4)
        return XR_RUNTIME_ABI_INVALID_SHAPE;
    if (facts->rc_offset != 0 || facts->object_kind_offset != 4 ||
        facts->flags_offset != 6 || facts->layout_id_offset != 8 ||
        facts->domain_id_offset != 12)
        return XR_RUNTIME_ABI_INVALID_SHAPE;
    if (facts->int32_twos_complement != 1 || facts->atomic_i32_lock_free != 1 ||
        facts->atomic_i32_rmw != 1 ||
        facts->atomic_order_mask != XR_RUNTIME_OBJECT_HEADER_REQUIRED_ATOMIC_ORDERS)
        return XR_RUNTIME_ABI_INVALID_POLICY;
    return XR_RUNTIME_ABI_OK;
}

static void set_field(XrRuntimePhysicalFieldAbi *field, uint16_t role, uint16_t offset,
                      uint16_t width, uint16_t alignment, uint8_t encoding,
                      uint8_t atomicity, uint8_t index_semantics) {
    *field = (XrRuntimePhysicalFieldAbi) {
        .role = role,
        .offset = offset,
        .width = width,
        .alignment = alignment,
        .encoding = encoding,
        .atomicity = atomicity,
        .index_semantics = index_semantics,
    };
}

XrRuntimeAbiStatus xr_runtime_object_kind_stable_id(uint16_t object_kind,
                                                    XrStableId *out) {
    if (!out)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    if (!object_kind_valid(object_kind))
        return XR_RUNTIME_ABI_INVALID_SHAPE;
    for (size_t i = 0; i < sizeof(canonical_object_kinds) /
                                      sizeof(canonical_object_kinds[0]);
         i++) {
        if (canonical_object_kinds[i].encoding == object_kind) {
            *out = canonical_object_kinds[i].stable_id;
            return XR_RUNTIME_ABI_OK;
        }
    }
    return XR_RUNTIME_ABI_INVALID_SHAPE;
}

XrRuntimeAbiStatus xr_runtime_object_header_native_materialization_facts(
    XrRuntimeObjectHeaderMaterializationFacts *out) {
    if (!out)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    XrRuntimeObjectHeaderMaterializationFacts facts;
    memset(&facts, 0, sizeof(facts));
    facts.schema_version = XR_RUNTIME_OBJECT_HEADER_FACTS_SCHEMA_VERSION;
    facts.header_size = (uint16_t) sizeof(XrRuntimeObjectHeader);
    facts.header_alignment = (uint16_t) _Alignof(XrRuntimeObjectHeader);
    facts.atomic_i32_size = (uint16_t) sizeof(_Atomic int32_t);
    facts.atomic_i32_alignment = (uint16_t) _Alignof(_Atomic int32_t);
    facts.uint16_size = (uint16_t) sizeof(uint16_t);
    facts.uint16_alignment = (uint16_t) _Alignof(uint16_t);
    facts.uint32_size = (uint16_t) sizeof(uint32_t);
    facts.uint32_alignment = (uint16_t) _Alignof(uint32_t);
    facts.rc_offset = (uint16_t) offsetof(XrRuntimeObjectHeader, rc);
    facts.object_kind_offset =
        (uint16_t) offsetof(XrRuntimeObjectHeader, object_kind);
    facts.flags_offset = (uint16_t) offsetof(XrRuntimeObjectHeader, flags);
    facts.layout_id_offset = (uint16_t) offsetof(XrRuntimeObjectHeader, layout_id);
    facts.domain_id_offset = (uint16_t) offsetof(XrRuntimeObjectHeader, domain_id);
    const uint16_t endian_probe = 1;
    facts.target_endian = *((const uint8_t *) &endian_probe) == 1
                              ? XR_RUNTIME_ENDIAN_LITTLE
                              : XR_RUNTIME_ENDIAN_BIG;
    facts.int32_twos_complement = INT32_MIN == (-INT32_MAX - 1) ? 1 : 0;
    _Atomic int32_t atomic_probe;
    atomic_init(&atomic_probe, 0);
    facts.atomic_i32_lock_free = atomic_is_lock_free(&atomic_probe) ? 1 : 0;
    facts.atomic_i32_rmw = 1;
    facts.atomic_order_mask = XR_RUNTIME_OBJECT_HEADER_REQUIRED_ATOMIC_ORDERS;
    *out = facts;
    return XR_RUNTIME_ABI_OK;
}

XrRuntimeAbiStatus xr_runtime_object_header_abi_materialize(
    const XrRuntimeObjectHeaderMaterializationFacts *facts,
    XrRuntimeObjectHeaderAbi *out) {
    if (!out)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    XrRuntimeAbiStatus status = verify_facts(facts);
    if (status != XR_RUNTIME_ABI_OK)
        return status;

    XrRuntimeObjectHeaderAbi candidate;
    memset(&candidate, 0, sizeof(candidate));
    candidate.schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION;
    candidate.size = facts->header_size;
    candidate.alignment = facts->header_alignment;
    candidate.target_endian = facts->target_endian;
    candidate.padding_policy = XR_RUNTIME_PADDING_MUST_BE_ZERO;

    set_field(&candidate.fields[0], XR_RUNTIME_FIELD_HEADER_RC, facts->rc_offset,
              facts->atomic_i32_size, facts->atomic_i32_alignment,
              XR_RUNTIME_FIELD_SIGNED_TWOS_COMPLEMENT,
              XR_RUNTIME_FIELD_DOMAIN_CONDITIONAL, XR_RUNTIME_INDEX_NONE);
    set_field(&candidate.fields[1], XR_RUNTIME_FIELD_HEADER_OBJECT_KIND,
              facts->object_kind_offset, facts->uint16_size, facts->uint16_alignment,
              XR_RUNTIME_FIELD_UNSIGNED, XR_RUNTIME_FIELD_PLAIN,
              XR_RUNTIME_INDEX_NONE);
    set_field(&candidate.fields[2], XR_RUNTIME_FIELD_HEADER_FLAGS,
              facts->flags_offset, facts->uint16_size, facts->uint16_alignment,
              XR_RUNTIME_FIELD_BITSET, XR_RUNTIME_FIELD_PLAIN,
              XR_RUNTIME_INDEX_NONE);
    set_field(&candidate.fields[3], XR_RUNTIME_FIELD_HEADER_LAYOUT_ID,
              facts->layout_id_offset, facts->uint32_size, facts->uint32_alignment,
              XR_RUNTIME_FIELD_UNSIGNED, XR_RUNTIME_FIELD_PLAIN,
              XR_RUNTIME_INDEX_VERIFIED_TABLE);
    set_field(&candidate.fields[4], XR_RUNTIME_FIELD_HEADER_DOMAIN_ID,
              facts->domain_id_offset, facts->uint32_size, facts->uint32_alignment,
              XR_RUNTIME_FIELD_UNSIGNED, XR_RUNTIME_FIELD_PLAIN,
              XR_RUNTIME_INDEX_VERIFIED_TABLE);

    candidate.rc = (XrRuntimeRcAbi) {
        .initial_value = XR_RUNTIME_OBJECT_RC_INITIAL,
        .retain_delta = XR_RUNTIME_OBJECT_RC_RETAIN_DELTA,
        .release_delta = XR_RUNTIME_OBJECT_RC_RELEASE_DELTA,
        .sticky_sentinel = XR_RUNTIME_OBJECT_RC_STICKY,
        .sticky_band_boundary = XR_RUNTIME_OBJECT_RC_STICKY_BAND,
        .polarity = XR_RUNTIME_RC_OWNED_POSITIVE,
        .sticky_comparison = XR_RUNTIME_RC_STICKY_LESS_OR_EQUAL,
        .local_access = XR_RUNTIME_RC_ACCESS_PLAIN,
        .shared_access = XR_RUNTIME_RC_ACCESS_ATOMIC,
        .shared_retain_order = XR_RUNTIME_MEMORY_ORDER_RELAXED,
        .shared_release_order = XR_RUNTIME_MEMORY_ORDER_RELEASE,
        .shared_destroy_order = XR_RUNTIME_MEMORY_ORDER_ACQUIRE,
    };

    candidate.object_kinds.invalid_encoding = XR_RUNTIME_OBJECT_KIND_INVALID;
    candidate.object_kinds.entry_count =
        (uint16_t) (sizeof(canonical_object_kinds) /
                    sizeof(canonical_object_kinds[0]));
    candidate.object_kinds.encoding_width = facts->uint16_size;
    memcpy(candidate.object_kinds.entries, canonical_object_kinds,
           sizeof(canonical_object_kinds));

    candidate.flags.valid_mask = XR_RUNTIME_OBJECT_FLAG_VALID_MASK;
    candidate.flags.reserved_zero_mask = UINT16_MAX;
    candidate.flags.entry_count = 0;
    candidate.flags.encoding_width = facts->uint16_size;

    candidate.layout_id.invalid_encoding = XR_RUNTIME_OBJECT_HEADER_INVALID_INDEX;
    candidate.layout_id.encoding_width = facts->uint32_size;
    candidate.layout_id.semantics = XR_RUNTIME_INDEX_VERIFIED_TABLE;
    candidate.domain_id.invalid_encoding = XR_RUNTIME_OBJECT_HEADER_INVALID_INDEX;
    candidate.domain_id.encoding_width = facts->uint32_size;
    candidate.domain_id.semantics = XR_RUNTIME_INDEX_VERIFIED_TABLE;

    XrFingerprint validation;
    status = xr_runtime_object_header_abi_fingerprint(&candidate, &validation);
    if (status != XR_RUNTIME_ABI_OK)
        return status;
    *out = candidate;
    return XR_RUNTIME_ABI_OK;
}

XrRuntimeAbiStatus xr_runtime_object_header_init(
    XrRuntimeObjectHeader *header, uint16_t object_kind, uint16_t flags,
    uint32_t layout_id, uint32_t domain_id) {
    if (!header)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    if (!object_kind_valid(object_kind))
        return XR_RUNTIME_ABI_INVALID_SHAPE;
    if (flags != XR_RUNTIME_OBJECT_FLAG_NONE)
        return XR_RUNTIME_ABI_INVALID_MASK;
    if (layout_id == XR_RUNTIME_OBJECT_HEADER_INVALID_INDEX ||
        domain_id == XR_RUNTIME_OBJECT_HEADER_INVALID_INDEX)
        return XR_RUNTIME_ABI_INVALID_SHAPE;

    atomic_init(&header->rc, XR_RUNTIME_OBJECT_RC_INITIAL);
    header->object_kind = object_kind;
    header->flags = flags;
    header->layout_id = layout_id;
    header->domain_id = domain_id;
    return XR_RUNTIME_ABI_OK;
}

XrRuntimeAbiStatus xr_runtime_object_header_validate(
    const XrRuntimeObjectHeader *header) {
    if (!header)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    if (!object_kind_valid(header->object_kind))
        return XR_RUNTIME_ABI_INVALID_SHAPE;
    if (header->flags != XR_RUNTIME_OBJECT_FLAG_NONE)
        return XR_RUNTIME_ABI_INVALID_MASK;
    if (header->layout_id == XR_RUNTIME_OBJECT_HEADER_INVALID_INDEX ||
        header->domain_id == XR_RUNTIME_OBJECT_HEADER_INVALID_INDEX)
        return XR_RUNTIME_ABI_INVALID_SHAPE;

    int32_t rc = atomic_load_explicit(&header->rc, memory_order_relaxed);
    if (rc <= 0 && rc > XR_RUNTIME_OBJECT_RC_STICKY_BAND)
        return XR_RUNTIME_ABI_INVALID_POLICY;
    return XR_RUNTIME_ABI_OK;
}
