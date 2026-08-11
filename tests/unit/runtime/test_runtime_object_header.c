/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_runtime_object_header.c - Canonical object-header ABI tests
 */

#include "runtime/abi/xr_runtime_object_header.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                    \
    do {                                                                                      \
        if (!(condition)) {                                                                   \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__,         \
                    #condition);                                                              \
            exit(1);                                                                          \
        }                                                                                     \
    } while (0)

typedef struct ExpectedKind {
    uint16_t encoding;
    const char *canonical_key;
} ExpectedKind;

static const ExpectedKind expected_kinds[] = {
    {XR_RUNTIME_OBJECT_KIND_STRING, "xray.runtime.object-kind.v1/string"},
    {XR_RUNTIME_OBJECT_KIND_CLOSURE, "xray.runtime.object-kind.v1/closure"},
    {XR_RUNTIME_OBJECT_KIND_BOXED_AGGREGATE,
     "xray.runtime.object-kind.v1/boxed-aggregate"},
    {XR_RUNTIME_OBJECT_KIND_ARRAY, "xray.runtime.object-kind.v1/array"},
    {XR_RUNTIME_OBJECT_KIND_MAP, "xray.runtime.object-kind.v1/map"},
    {XR_RUNTIME_OBJECT_KIND_SET, "xray.runtime.object-kind.v1/set"},
    {XR_RUNTIME_OBJECT_KIND_INSTANCE, "xray.runtime.object-kind.v1/instance"},
    {XR_RUNTIME_OBJECT_KIND_ENUM_BOX, "xray.runtime.object-kind.v1/enum-box"},
    {XR_RUNTIME_OBJECT_KIND_CELL, "xray.runtime.object-kind.v1/cell"},
};

static bool fingerprint_equal(XrFingerprint left, XrFingerprint right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static void require_bytes(const uint8_t *actual, const uint8_t *expected, size_t size) {
    if (memcmp(actual, expected, size) == 0)
        return;
    fprintf(stderr, "byte mismatch: ");
    for (size_t i = 0; i < size; i++)
        fprintf(stderr, "%02x", actual[i]);
    fputc('\n', stderr);
    exit(1);
}

static XrStableId policy_stable_id(const char *canonical_key) {
    static const uint8_t domain[] = "xray-entity-id-v1\0";
    size_t key_size = strlen(canonical_key);
    REQUIRE(key_size <= UINT32_MAX);
    uint32_t width = (uint32_t) key_size;
    uint8_t encoded_width[4];
    for (size_t i = 0; i < sizeof(encoded_width); i++)
        encoded_width[i] = (uint8_t) (width >> (i * 8));

    uint8_t digest[XR_FINGERPRINT_BYTES];
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1);
    xr_sha256_update(&ctx, encoded_width, sizeof(encoded_width));
    xr_sha256_update(&ctx, (const uint8_t *) canonical_key, key_size);
    xr_sha256_final(&ctx, digest);
    XrStableId id;
    memcpy(id.bytes, digest, sizeof(id.bytes));
    return id;
}

static XrRuntimeObjectHeaderMaterializationFacts target_facts(uint8_t endian) {
    return (XrRuntimeObjectHeaderMaterializationFacts) {
        .schema_version = XR_RUNTIME_OBJECT_HEADER_FACTS_SCHEMA_VERSION,
        .header_size = 16,
        .header_alignment = 4,
        .atomic_i32_size = 4,
        .atomic_i32_alignment = 4,
        .uint16_size = 2,
        .uint16_alignment = 2,
        .uint32_size = 4,
        .uint32_alignment = 4,
        .rc_offset = 0,
        .object_kind_offset = 4,
        .flags_offset = 6,
        .layout_id_offset = 8,
        .domain_id_offset = 12,
        .target_endian = endian,
        .int32_twos_complement = 1,
        .atomic_i32_lock_free = 1,
        .atomic_i32_rmw = 1,
        .atomic_order_mask = XR_RUNTIME_OBJECT_HEADER_REQUIRED_ATOMIC_ORDERS,
    };
}

static XrRuntimeObjectHeaderAbi canonical_abi(void) {
    XrRuntimeObjectHeaderMaterializationFacts facts =
        target_facts(XR_RUNTIME_ENDIAN_LITTLE);
    XrRuntimeObjectHeaderAbi abi;
    REQUIRE(xr_runtime_object_header_abi_materialize(&facts, &abi) ==
            XR_RUNTIME_ABI_OK);
    return abi;
}

static void test_native_and_target_facts_agree(void) {
    XrRuntimeObjectHeaderMaterializationFacts native;
    REQUIRE(xr_runtime_object_header_native_materialization_facts(&native) ==
            XR_RUNTIME_ABI_OK);
    REQUIRE(native.target_endian == XR_RUNTIME_ENDIAN_LITTLE);
    XrRuntimeObjectHeaderMaterializationFacts target = target_facts(native.target_endian);
    REQUIRE(native.schema_version == target.schema_version);
    REQUIRE(native.header_size == target.header_size);
    REQUIRE(native.header_alignment == target.header_alignment);
    REQUIRE(native.atomic_i32_size == target.atomic_i32_size);
    REQUIRE(native.atomic_i32_alignment == target.atomic_i32_alignment);
    REQUIRE(native.uint16_size == target.uint16_size);
    REQUIRE(native.uint16_alignment == target.uint16_alignment);
    REQUIRE(native.uint32_size == target.uint32_size);
    REQUIRE(native.uint32_alignment == target.uint32_alignment);
    REQUIRE(native.rc_offset == target.rc_offset);
    REQUIRE(native.object_kind_offset == target.object_kind_offset);
    REQUIRE(native.flags_offset == target.flags_offset);
    REQUIRE(native.layout_id_offset == target.layout_id_offset);
    REQUIRE(native.domain_id_offset == target.domain_id_offset);
    REQUIRE(native.int32_twos_complement == target.int32_twos_complement);
    REQUIRE(native.atomic_i32_lock_free == target.atomic_i32_lock_free);
    REQUIRE(native.atomic_i32_rmw == target.atomic_i32_rmw);
    REQUIRE(native.atomic_order_mask == target.atomic_order_mask);
    REQUIRE(native.reserved32 == 0 && native.reserved[0] == 0 &&
            native.reserved[1] == 0);

    XrRuntimeObjectHeaderAbi native_abi;
    XrRuntimeObjectHeaderAbi target_abi;
    REQUIRE(xr_runtime_object_header_abi_materialize(&native, &native_abi) ==
            XR_RUNTIME_ABI_OK);
    REQUIRE(xr_runtime_object_header_abi_materialize(&target, &target_abi) ==
            XR_RUNTIME_ABI_OK);
    XrFingerprint native_fingerprint;
    XrFingerprint target_fingerprint;
    REQUIRE(xr_runtime_object_header_abi_fingerprint(&native_abi,
                                                     &native_fingerprint) ==
            XR_RUNTIME_ABI_OK);
    REQUIRE(xr_runtime_object_header_abi_fingerprint(&target_abi,
                                                     &target_fingerprint) ==
            XR_RUNTIME_ABI_OK);
    REQUIRE(fingerprint_equal(native_fingerprint, target_fingerprint));
}

static void test_layout_and_registry(void) {
    XrRuntimeObjectHeaderAbi abi = canonical_abi();
    REQUIRE(abi.size == XR_RUNTIME_OBJECT_HEADER_SIZE);
    REQUIRE(abi.alignment == XR_RUNTIME_OBJECT_HEADER_ALIGNMENT);
    REQUIRE(abi.fields[0].offset == offsetof(XrRuntimeObjectHeader, rc));
    REQUIRE(abi.fields[1].offset == offsetof(XrRuntimeObjectHeader, object_kind));
    REQUIRE(abi.fields[2].offset == offsetof(XrRuntimeObjectHeader, flags));
    REQUIRE(abi.fields[3].offset == offsetof(XrRuntimeObjectHeader, layout_id));
    REQUIRE(abi.fields[4].offset == offsetof(XrRuntimeObjectHeader, domain_id));
    REQUIRE(abi.rc.initial_value == XR_RUNTIME_OBJECT_RC_INITIAL);
    REQUIRE(abi.rc.sticky_sentinel == XR_RUNTIME_OBJECT_RC_STICKY);
    REQUIRE(abi.rc.sticky_band_boundary == XR_RUNTIME_OBJECT_RC_STICKY_BAND);
    REQUIRE(abi.object_kinds.entry_count ==
            sizeof(expected_kinds) / sizeof(expected_kinds[0]));
    REQUIRE(abi.flags.entry_count == 0);
    REQUIRE(abi.flags.valid_mask == 0);
    REQUIRE(abi.flags.reserved_zero_mask == UINT16_MAX);
    REQUIRE(abi.layout_id.invalid_encoding == UINT32_MAX);
    REQUIRE(abi.domain_id.invalid_encoding == UINT32_MAX);

    XrStableId ids[XR_RUNTIME_OBJECT_KIND_COUNT - 1];
    for (size_t i = 0; i < sizeof(expected_kinds) / sizeof(expected_kinds[0]); i++) {
        REQUIRE(xr_runtime_object_kind_stable_id(expected_kinds[i].encoding,
                                                 &ids[i]) == XR_RUNTIME_ABI_OK);
        XrStableId expected = policy_stable_id(expected_kinds[i].canonical_key);
        require_bytes(ids[i].bytes, expected.bytes, sizeof(expected.bytes));
        for (size_t j = 0; j < i; j++)
            REQUIRE(memcmp(ids[i].bytes, ids[j].bytes, sizeof(ids[i].bytes)) != 0);
    }
    for (size_t i = 1; i < abi.object_kinds.entry_count; i++)
        REQUIRE(memcmp(abi.object_kinds.entries[i - 1].stable_id.bytes,
                       abi.object_kinds.entries[i].stable_id.bytes,
                       XR_STABLE_ID_BYTES) < 0);
}

static void test_fingerprint_kat_and_mutations(void) {
    static const uint8_t expected[XR_FINGERPRINT_BYTES] = {
        0xdb, 0xc2, 0x22, 0xe3, 0x1c, 0x79, 0x03, 0x6f,
        0xf4, 0x67, 0x98, 0x38, 0x21, 0xb5, 0x8c, 0x6e,
        0xe8, 0x99, 0xb4, 0x35, 0xef, 0x8c, 0xfa, 0xa4,
        0xb7, 0x62, 0x9b, 0xdd, 0x8b, 0x79, 0x4f, 0xf3,
    };
    XrRuntimeObjectHeaderAbi abi = canonical_abi();
    XrFingerprint actual;
    REQUIRE(xr_runtime_object_header_abi_fingerprint(&abi, &actual) ==
            XR_RUNTIME_ABI_OK);
    require_bytes(actual.bytes, expected, sizeof(expected));

    XrRuntimeObjectHeaderMaterializationFacts big_facts =
        target_facts(XR_RUNTIME_ENDIAN_BIG);
    XrRuntimeObjectHeaderAbi big_endian;
    XrFingerprint big_endian_fingerprint;
    REQUIRE(xr_runtime_object_header_abi_materialize(&big_facts, &big_endian) ==
            XR_RUNTIME_ABI_OK);
    REQUIRE(xr_runtime_object_header_abi_fingerprint(&big_endian,
                                                     &big_endian_fingerprint) ==
            XR_RUNTIME_ABI_OK);
    REQUIRE(!fingerprint_equal(actual, big_endian_fingerprint));

    XrRuntimeObjectHeaderAbi mutated = abi;
    mutated.fields[2].offset = mutated.fields[1].offset;
    REQUIRE(xr_runtime_object_header_abi_fingerprint(&mutated, &actual) ==
            XR_RUNTIME_ABI_INVALID_OVERLAP);
    mutated = abi;
    memset(&mutated.object_kinds.entries[0].stable_id, 0,
           sizeof(mutated.object_kinds.entries[0].stable_id));
    REQUIRE(xr_runtime_object_header_abi_fingerprint(&mutated, &actual) ==
            XR_RUNTIME_ABI_INVALID_SHAPE);
    mutated = abi;
    mutated.object_kinds.entries[1].stable_id =
        mutated.object_kinds.entries[0].stable_id;
    REQUIRE(xr_runtime_object_header_abi_fingerprint(&mutated, &actual) ==
            XR_RUNTIME_ABI_INVALID_ORDER);
    mutated = abi;
    mutated.object_kinds.entries[1].encoding =
        mutated.object_kinds.entries[0].encoding;
    REQUIRE(xr_runtime_object_header_abi_fingerprint(&mutated, &actual) ==
            XR_RUNTIME_ABI_INVALID_ORDER);
    mutated = abi;
    mutated.flags.valid_mask = 1;
    REQUIRE(xr_runtime_object_header_abi_fingerprint(&mutated, &actual) ==
            XR_RUNTIME_ABI_INVALID_MASK);
    mutated = abi;
    mutated.reserved[0] = 1;
    REQUIRE(xr_runtime_object_header_abi_fingerprint(&mutated, &actual) ==
            XR_RUNTIME_ABI_INVALID_POLICY);
}

static void require_facts_rejected(
    XrRuntimeObjectHeaderMaterializationFacts facts,
    XrRuntimeAbiStatus expected_status) {
    XrRuntimeObjectHeaderAbi output;
    memset(&output, 0xa5, sizeof(output));
    XrRuntimeObjectHeaderAbi unchanged = output;
    REQUIRE(xr_runtime_object_header_abi_materialize(&facts, &output) ==
            expected_status);
    REQUIRE(memcmp(&output, &unchanged, sizeof(output)) == 0);
}

static void test_materialization_fact_mutations(void) {
    XrRuntimeObjectHeaderMaterializationFacts facts =
        target_facts(XR_RUNTIME_ENDIAN_LITTLE);
    XrRuntimeObjectHeaderMaterializationFacts mutated = facts;
    mutated.schema_version++;
    require_facts_rejected(mutated, XR_RUNTIME_ABI_INVALID_SCHEMA);
    mutated = facts;
    mutated.header_size = 20;
    require_facts_rejected(mutated, XR_RUNTIME_ABI_INVALID_SHAPE);
    mutated = facts;
    mutated.header_alignment = 8;
    require_facts_rejected(mutated, XR_RUNTIME_ABI_INVALID_SHAPE);
    mutated = facts;
    mutated.atomic_i32_size = 8;
    require_facts_rejected(mutated, XR_RUNTIME_ABI_INVALID_SHAPE);
    mutated = facts;
    mutated.atomic_i32_alignment = 8;
    require_facts_rejected(mutated, XR_RUNTIME_ABI_INVALID_SHAPE);
    mutated = facts;
    mutated.atomic_i32_lock_free = 0;
    require_facts_rejected(mutated, XR_RUNTIME_ABI_INVALID_POLICY);
    mutated = facts;
    mutated.atomic_i32_rmw = 0;
    require_facts_rejected(mutated, XR_RUNTIME_ABI_INVALID_POLICY);
    mutated = facts;
    mutated.atomic_order_mask &= ~XR_RUNTIME_ATOMIC_ORDER_ACQUIRE;
    require_facts_rejected(mutated, XR_RUNTIME_ABI_INVALID_POLICY);
    mutated = facts;
    mutated.atomic_order_mask |= UINT32_C(1) << 31;
    require_facts_rejected(mutated, XR_RUNTIME_ABI_INVALID_POLICY);
    mutated = facts;
    mutated.int32_twos_complement = 0;
    require_facts_rejected(mutated, XR_RUNTIME_ABI_INVALID_POLICY);
    mutated = facts;
    mutated.uint16_size = 4;
    require_facts_rejected(mutated, XR_RUNTIME_ABI_INVALID_SHAPE);
    mutated = facts;
    mutated.uint16_alignment = 4;
    require_facts_rejected(mutated, XR_RUNTIME_ABI_INVALID_SHAPE);
    mutated = facts;
    mutated.uint32_size = 8;
    require_facts_rejected(mutated, XR_RUNTIME_ABI_INVALID_SHAPE);
    mutated = facts;
    mutated.uint32_alignment = 8;
    require_facts_rejected(mutated, XR_RUNTIME_ABI_INVALID_SHAPE);
    mutated = facts;
    mutated.rc_offset = 4;
    require_facts_rejected(mutated, XR_RUNTIME_ABI_INVALID_SHAPE);
    mutated = facts;
    mutated.object_kind_offset = 6;
    require_facts_rejected(mutated, XR_RUNTIME_ABI_INVALID_SHAPE);
    mutated = facts;
    mutated.flags_offset = 8;
    require_facts_rejected(mutated, XR_RUNTIME_ABI_INVALID_SHAPE);
    mutated = facts;
    mutated.layout_id_offset = 12;
    require_facts_rejected(mutated, XR_RUNTIME_ABI_INVALID_SHAPE);
    mutated = facts;
    mutated.domain_id_offset = 8;
    require_facts_rejected(mutated, XR_RUNTIME_ABI_INVALID_SHAPE);
    mutated = facts;
    mutated.reserved32 = 1;
    require_facts_rejected(mutated, XR_RUNTIME_ABI_INVALID_POLICY);
    mutated = facts;
    mutated.reserved[1] = 1;
    require_facts_rejected(mutated, XR_RUNTIME_ABI_INVALID_POLICY);
    mutated = facts;
    mutated.target_endian = XR_RUNTIME_ENDIAN_INVALID;
    require_facts_rejected(mutated, XR_RUNTIME_ABI_INVALID_POLICY);
    XrRuntimeObjectHeaderAbi output;
    REQUIRE(xr_runtime_object_header_abi_materialize(NULL, &output) ==
            XR_RUNTIME_ABI_INVALID_ARGUMENT);
    REQUIRE(xr_runtime_object_header_abi_materialize(&facts, NULL) ==
            XR_RUNTIME_ABI_INVALID_ARGUMENT);
}

static void test_header_initialization_and_validation(void) {
    XrRuntimeObjectHeader header;
    REQUIRE(xr_runtime_object_header_init(
                &header, XR_RUNTIME_OBJECT_KIND_STRING,
                XR_RUNTIME_OBJECT_FLAG_NONE, 0, 7) == XR_RUNTIME_ABI_OK);
    REQUIRE(atomic_load_explicit(&header.rc, memory_order_relaxed) ==
            XR_RUNTIME_OBJECT_RC_INITIAL);
    REQUIRE(header.object_kind == XR_RUNTIME_OBJECT_KIND_STRING);
    REQUIRE(header.flags == XR_RUNTIME_OBJECT_FLAG_NONE);
    REQUIRE(header.layout_id == 0);
    REQUIRE(header.domain_id == 7);
    REQUIRE(xr_runtime_object_header_validate(&header) == XR_RUNTIME_ABI_OK);

    atomic_store_explicit(&header.rc, XR_RUNTIME_OBJECT_RC_STICKY,
                          memory_order_relaxed);
    REQUIRE(xr_runtime_object_header_validate(&header) == XR_RUNTIME_ABI_OK);
    atomic_store_explicit(&header.rc, -1, memory_order_relaxed);
    REQUIRE(xr_runtime_object_header_validate(&header) ==
            XR_RUNTIME_ABI_INVALID_POLICY);
    atomic_store_explicit(&header.rc, XR_RUNTIME_OBJECT_RC_INITIAL,
                          memory_order_relaxed);
    header.object_kind = XR_RUNTIME_OBJECT_KIND_INVALID;
    REQUIRE(xr_runtime_object_header_validate(&header) ==
            XR_RUNTIME_ABI_INVALID_SHAPE);
    header.object_kind = XR_RUNTIME_OBJECT_KIND_STRING;
    header.flags = UINT16_C(1);
    REQUIRE(xr_runtime_object_header_validate(&header) ==
            XR_RUNTIME_ABI_INVALID_MASK);
    header.flags = XR_RUNTIME_OBJECT_FLAG_NONE;
    header.layout_id = XR_RUNTIME_OBJECT_HEADER_INVALID_INDEX;
    REQUIRE(xr_runtime_object_header_validate(&header) ==
            XR_RUNTIME_ABI_INVALID_SHAPE);

    REQUIRE(xr_runtime_object_header_init(
                &header, XR_RUNTIME_OBJECT_KIND_INVALID,
                XR_RUNTIME_OBJECT_FLAG_NONE, 0, 0) == XR_RUNTIME_ABI_INVALID_SHAPE);
    REQUIRE(xr_runtime_object_header_init(
                &header, XR_RUNTIME_OBJECT_KIND_COUNT,
                XR_RUNTIME_OBJECT_FLAG_NONE, 0, 0) == XR_RUNTIME_ABI_INVALID_SHAPE);
    REQUIRE(xr_runtime_object_header_init(
                &header, XR_RUNTIME_OBJECT_KIND_STRING, UINT16_C(1), 0,
                0) == XR_RUNTIME_ABI_INVALID_MASK);
    REQUIRE(xr_runtime_object_header_init(
                &header, XR_RUNTIME_OBJECT_KIND_STRING,
                XR_RUNTIME_OBJECT_FLAG_NONE, XR_RUNTIME_OBJECT_HEADER_INVALID_INDEX,
                0) == XR_RUNTIME_ABI_INVALID_SHAPE);
}

static void test_lookup_output_publication(void) {
    XrStableId id;
    memset(&id, 0xa5, sizeof(id));
    XrStableId unchanged = id;
    REQUIRE(xr_runtime_object_kind_stable_id(XR_RUNTIME_OBJECT_KIND_INVALID,
                                             &id) == XR_RUNTIME_ABI_INVALID_SHAPE);
    REQUIRE(memcmp(&id, &unchanged, sizeof(id)) == 0);
    REQUIRE(xr_runtime_object_kind_stable_id(XR_RUNTIME_OBJECT_KIND_COUNT,
                                             &id) == XR_RUNTIME_ABI_INVALID_SHAPE);
    REQUIRE(memcmp(&id, &unchanged, sizeof(id)) == 0);
}

int main(void) {
    test_native_and_target_facts_agree();
    test_layout_and_registry();
    test_fingerprint_kat_and_mutations();
    test_materialization_fact_mutations();
    test_header_initialization_and_validation();
    test_lookup_output_publication();
    puts("runtime object header tests passed");
    return 0;
}
