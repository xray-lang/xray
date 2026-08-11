/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_runtime_string_object.c - Canonical string-object contract tests
 */

#include "runtime/abi/xr_runtime_string_object.h"
#include "plan/semantic/xr_semantic_ids.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                   \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, \
                    __LINE__, #condition);                                   \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static bool fingerprint_equal(XrFingerprint left, XrFingerprint right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static void require_stable_id(const char *key, XrStableId actual) {
    XrStableId expected;
    XrFingerprint digest;
    REQUIRE(xr_stable_id_from_key(key, &expected, &digest));
    REQUIRE(memcmp(expected.bytes, actual.bytes, sizeof(expected.bytes)) == 0);
}

static XrRuntimeStringObjectContract canonical_contract(void) {
    XrRuntimeStringObjectContract contract;
    REQUIRE(xr_runtime_string_object_contract_build(&contract) ==
            XR_RUNTIME_ABI_OK);
    REQUIRE(xr_runtime_string_object_contract_verify(&contract) ==
            XR_RUNTIME_ABI_OK);
    return contract;
}

static void test_contract_shape_and_extent(void) {
    XrRuntimeStringObjectContract contract = canonical_contract();
    REQUIRE(contract.schema_version ==
            XR_RUNTIME_STRING_CONTRACT_SCHEMA_VERSION);
    REQUIRE(contract.layout_index == XR_RUNTIME_STRING_LAYOUT_INDEX);
    REQUIRE(contract.data_offset == offsetof(XrString, data));
    REQUIRE(contract.rune_length_unknown == UINT32_MAX);
    REQUIRE(contract.layout.fixed_prefix_size == sizeof(XrString));
    REQUIRE(contract.extent.tail_offset == sizeof(XrString));
    REQUIRE(contract.extent.stride == 1);
    REQUIRE(contract.extent.operand_index == 0);
    REQUIRE(contract.layout.object_kind_id.bytes[0] != 0);
    REQUIRE(contract.layout.flags ==
            (XR_LAYOUT_HAS_CLONE | XR_LAYOUT_HAS_EQ_HASH));
    REQUIRE(contract.layout.destructor_id.bytes[0] == 0);
    REQUIRE(contract.layout.clone_id.bytes[0] != 0);
    REQUIRE(contract.layout.eq_hash_id.bytes[0] != 0);
    REQUIRE(contract.fields[0].role ==
            XR_RUNTIME_STRING_FIELD_OBJECT_HEADER);
    REQUIRE(contract.fields[0].width == XR_RUNTIME_OBJECT_HEADER_SIZE);
    REQUIRE(contract.fields[5].role == XR_RUNTIME_STRING_FIELD_UTF8_TAIL);
    REQUIRE(contract.fields[5].flags ==
            XR_RUNTIME_STRING_FIELD_FLEXIBLE_TAIL);
    REQUIRE(contract.materializations[0].kind ==
            XR_RUNTIME_STRING_MATERIALIZATION_LITERAL_VIEW);
    REQUIRE(contract.materializations[0].has_object_header == 0);
    REQUIRE(contract.materializations[1].kind ==
            XR_RUNTIME_STRING_MATERIALIZATION_OWNED_OBJECT);
    REQUIRE(contract.materializations[1].has_object_header == 1);
    REQUIRE(contract.literal_view.schema_version ==
            XR_RUNTIME_STRING_LITERAL_CONTRACT_SCHEMA_VERSION);
    REQUIRE(contract.literal_view.dynamic_tag ==
            XR_RUNTIME_STRING_LITERAL_DYNAMIC_TAG);
    REQUIRE(contract.literal_view.literal_flag ==
            XR_RUNTIME_STRING_LITERAL_FLAG);
    REQUIRE(contract.literal_view.semantic_domain ==
            XR_STORAGE_CONST_SHARED);
    REQUIRE(contract.literal_view.backend_materialization ==
            XR_MATERIALIZE_STATIC_DATA);
    REQUIRE(contract.literal_view.view_size ==
            sizeof(XrRuntimeStringLiteralView));
    REQUIRE(contract.literal_view.view_alignment ==
            _Alignof(XrRuntimeStringLiteralView));
    REQUIRE(contract.literal_view.fields[4].offset ==
            offsetof(XrRuntimeStringLiteralView, data));
    REQUIRE(xr_runtime_string_literal_materialization_contract_verify(
                &contract.literal_view) == XR_RUNTIME_ABI_OK);

    XrRuntimeEvaluatedExtent extent;
    XrRuntimeExtentLimits limits = {
        .max_allocation_bytes = UINT64_C(1) << 30,
        .max_alignment = 64,
    };
    REQUIRE(xr_runtime_string_object_extent_for_length(
                &contract, 37, limits, &extent) == XR_RUNTIME_ABI_OK);
    REQUIRE(extent.bytes == 72);
    REQUIRE(extent.alignment == XR_RUNTIME_OBJECT_HEADER_ALIGNMENT);
    limits.max_allocation_bytes = 71;
    REQUIRE(xr_runtime_string_object_extent_for_length(
                &contract, 37, limits, &extent) ==
            XR_RUNTIME_ABI_LIMIT_EXCEEDED);
    limits.max_allocation_bytes = UINT64_MAX;
    REQUIRE(xr_runtime_string_object_extent_for_length(
                &contract,
                XR_RUNTIME_STRING_MAXIMUM_BYTE_LENGTH + UINT32_C(1),
                limits, &extent) == XR_RUNTIME_ABI_LIMIT_EXCEEDED);

    for (uint32_t i = 0; i < contract.domain_count; i++) {
        REQUIRE(xr_runtime_domain_identity_valid(contract.domains[i]));
        REQUIRE(xr_runtime_layout_allows_domain(&contract.layout,
                                                contract.domains[i]));
    }
    REQUIRE(contract.domains[XR_RUNTIME_STRING_DOMAIN_EXEC_LOCAL]
                .materialization == XR_MATERIALIZE_EXEC_HEAP);
    REQUIRE(contract.domains[XR_RUNTIME_STRING_DOMAIN_CONST_SHARED]
                .materialization == XR_MATERIALIZE_SYSTEM_HEAP);
}

static void test_stable_id_provenance(void) {
    static const char *domain_keys[XR_RUNTIME_STRING_DOMAIN_COUNT] = {
        "runtime.domain.string.exec-local.v1",
        "runtime.domain.string.transferable.v1",
        "runtime.domain.string.const-shared.v1",
        "runtime.domain.string.sync-shared.v1",
    };
    XrRuntimeStringObjectContract contract = canonical_contract();
    require_stable_id("runtime.extent.string.inline-utf8.v1",
                      contract.extent.id);
    require_stable_id("runtime.layout.string.inline-utf8.v1",
                      contract.extent.layout_id);
    require_stable_id("runtime.layout.string.inline-utf8.v1",
                      contract.layout.layout_id);
    require_stable_id("runtime.layout-descriptor.string.inline-utf8.v1",
                      contract.layout.descriptor_id);
    require_stable_id("runtime.clone.string.inline-utf8.v1",
                      contract.layout.clone_id);
    require_stable_id("runtime.eq-hash.string.utf8-bytes.v1",
                      contract.layout.eq_hash_id);
    for (uint32_t i = 0; i < XR_RUNTIME_STRING_DOMAIN_COUNT; i++)
        require_stable_id(domain_keys[i], contract.domains[i].contract_id);
}

static void test_contract_mutations(void) {
    XrRuntimeStringObjectContract contract = canonical_contract();
    XrRuntimeStringObjectContract mutated = contract;
    mutated.schema_version++;
    REQUIRE(xr_runtime_string_object_contract_verify(&mutated) ==
            XR_RUNTIME_ABI_INVALID_SCHEMA);

    mutated = contract;
    mutated.data_offset++;
    REQUIRE(xr_runtime_string_object_contract_verify(&mutated) ==
            XR_RUNTIME_ABI_INVALID_SHAPE);
    mutated = contract;
    mutated.rune_length_unknown = 0;
    REQUIRE(xr_runtime_string_object_contract_verify(&mutated) ==
            XR_RUNTIME_ABI_INVALID_SHAPE);
    mutated = contract;
    mutated.trait_valid_mask ^= XR_RUNTIME_STRING_TRAIT_LONG;
    REQUIRE(xr_runtime_string_object_contract_verify(&mutated) ==
            XR_RUNTIME_ABI_INVALID_MASK);
    mutated = contract;
    mutated.fields[2].offset = mutated.fields[1].offset;
    REQUIRE(xr_runtime_string_object_contract_verify(&mutated) ==
            XR_RUNTIME_ABI_INVALID_OVERLAP);
    mutated = contract;
    mutated.materializations[0].has_object_header = 1;
    REQUIRE(xr_runtime_string_object_contract_verify(&mutated) ==
            XR_RUNTIME_ABI_INVALID_POLICY);
    mutated = contract;
    mutated.literal_view.dynamic_tag++;
    REQUIRE(xr_runtime_string_object_contract_verify(&mutated) !=
            XR_RUNTIME_ABI_OK);
    mutated = contract;
    mutated.literal_view.fields[4].offset++;
    REQUIRE(xr_runtime_string_object_contract_verify(&mutated) !=
            XR_RUNTIME_ABI_OK);
    mutated = contract;
    mutated.literal_view.fingerprint.bytes[0] ^= 1;
    REQUIRE(xr_runtime_string_object_contract_verify(&mutated) ==
            XR_RUNTIME_ABI_FINGERPRINT_MISMATCH);
    mutated = contract;
    mutated.reserved[1] = 1;
    REQUIRE(xr_runtime_string_object_contract_verify(&mutated) ==
            XR_RUNTIME_ABI_INVALID_POLICY);
    mutated = contract;
    mutated.extent.tail_offset++;
    REQUIRE(xr_runtime_string_object_contract_verify(&mutated) !=
            XR_RUNTIME_ABI_OK);
    mutated = contract;
    mutated.layout.alignment = 8;
    REQUIRE(xr_runtime_string_object_contract_verify(&mutated) !=
            XR_RUNTIME_ABI_OK);
    mutated = contract;
    mutated.domains[XR_RUNTIME_STRING_DOMAIN_CONST_SHARED].materialization =
        XR_MATERIALIZE_STATIC_DATA;
    REQUIRE(xr_runtime_string_object_contract_verify(&mutated) !=
            XR_RUNTIME_ABI_OK);
    mutated = contract;
    mutated.domains[1] = mutated.domains[0];
    REQUIRE(xr_runtime_string_object_contract_verify(&mutated) !=
            XR_RUNTIME_ABI_OK);
    mutated = contract;
    mutated.fingerprint.bytes[17] ^= UINT8_C(0x80);
    REQUIRE(xr_runtime_string_object_contract_verify(&mutated) ==
            XR_RUNTIME_ABI_FINGERPRINT_MISMATCH);
}

static void test_object_validation_and_rc(void) {
    const char bytes[] = "canonical";
    size_t allocation_size = (size_t) xr_runtime_string_object_allocation_bytes(
        (uint32_t) (sizeof(bytes) - 1));
    XrString *string = (XrString *) calloc(1, allocation_size);
    REQUIRE(string != NULL);
    REQUIRE(xr_runtime_string_object_init(
                string, XR_RUNTIME_STRING_DOMAIN_EXEC_LOCAL,
                (uint32_t) (sizeof(bytes) - 1), UINT32_MAX, 0,
                XR_RUNTIME_STRING_TRAIT_LOCAL) == XR_RUNTIME_ABI_OK);
    memcpy(string->data, bytes, sizeof(bytes));
    REQUIRE(xr_runtime_string_object_validate_prefix(string) ==
            XR_RUNTIME_ABI_OK);
    string->rune_length = (uint32_t) (sizeof(bytes) - 1);
    REQUIRE(xr_runtime_string_object_validate(string, allocation_size) ==
            XR_RUNTIME_ABI_OK);

    REQUIRE(xr_runtime_object_header_retain(&string->header) ==
            XR_RUNTIME_ABI_OK);
    REQUIRE(atomic_load_explicit(&string->header.rc, memory_order_relaxed) == 2);
    bool last = true;
    REQUIRE(xr_runtime_object_header_release(&string->header, &last) ==
            XR_RUNTIME_ABI_OK && !last);
    REQUIRE(xr_runtime_object_header_release(&string->header, &last) ==
            XR_RUNTIME_ABI_OK && last);
    REQUIRE(atomic_load_explicit(&string->header.rc, memory_order_relaxed) ==
            XR_RUNTIME_OBJECT_RC_STICKY);
    REQUIRE(xr_runtime_object_header_retain(&string->header) ==
            XR_RUNTIME_ABI_OK);

    string->reserved16 = 1;
    REQUIRE(xr_runtime_string_object_validate_prefix(string) ==
            XR_RUNTIME_ABI_INVALID_SHAPE);
    REQUIRE(xr_runtime_string_object_validate(string, allocation_size) ==
            XR_RUNTIME_ABI_INVALID_SHAPE);
    string->reserved16 = 0;
    atomic_fetch_or_explicit(&string->traits, UINT16_C(0x8000),
                             memory_order_relaxed);
    REQUIRE(xr_runtime_string_object_validate_prefix(string) ==
            XR_RUNTIME_ABI_INVALID_POLICY);
    atomic_fetch_and_explicit(&string->traits, UINT16_C(0x7fff),
                              memory_order_relaxed);
    string->data[string->length] = 'x';
    REQUIRE(xr_runtime_string_object_validate(string, allocation_size) ==
            XR_RUNTIME_ABI_INVALID_EXTENT);
    free(string);
}

static void test_fingerprint_is_stable(void) {
    static const uint8_t expected[XR_FINGERPRINT_BYTES] = {
        0xf6, 0xbc, 0xfa, 0x2a, 0x70, 0xd8, 0x0d, 0xf3,
        0xff, 0xb7, 0x0a, 0x3a, 0x2d, 0xbe, 0xfa, 0x79,
        0x5c, 0x46, 0x3a, 0xfd, 0xf6, 0xb0, 0x38, 0x89,
        0x13, 0xe2, 0x7a, 0xd1, 0x35, 0xe2, 0x8a, 0x28,
    };
    XrRuntimeStringObjectContract contract = canonical_contract();
    if (memcmp(contract.fingerprint.bytes, expected, sizeof(expected)) != 0) {
        for (size_t i = 0; i < sizeof(expected); i++)
            fprintf(stderr, "%02x", contract.fingerprint.bytes[i]);
        fputc('\n', stderr);
        exit(2);
    }
    XrFingerprint rebuilt;
    REQUIRE(xr_runtime_string_object_contract_fingerprint(&contract,
                                                           &rebuilt) ==
            XR_RUNTIME_ABI_OK);
    REQUIRE(fingerprint_equal(contract.fingerprint, rebuilt));
}

int main(void) {
    test_contract_shape_and_extent();
    test_stable_id_provenance();
    test_contract_mutations();
    test_object_validation_and_rc();
    test_fingerprint_is_stable();
    puts("runtime string object contract tests passed");
    return 0;
}
