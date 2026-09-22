/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_provider_logical_contract.c - Logical identity and hostile wire checks
 */

#include "runtime/abi/xr_provider_logical_contract.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, message)                                                                  \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL: %s\n", message);                                                \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static XrProviderLogicalContract make_clock(void) {
    return (XrProviderLogicalContract) {
        .schema_version = XR_PROVIDER_LOGICAL_SCHEMA_VERSION,
        .effects = XR_PROVIDER_EFFECT_READS_CLOCK,
        .platforms =
            XR_PROVIDER_PLATFORM_LINUX | XR_PROVIDER_PLATFORM_MACOS | XR_PROVIDER_PLATFORM_WINDOWS,
        .runtime_profiles = XR_PROVIDER_LOGICAL_PROFILE_HOSTED,
        .type_byte_count = 2u,
        .result_owner = XR_PROVIDER_OWNER_TRIVIAL,
        .error_owner = XR_PROVIDER_OWNER_TRIVIAL,
        .threads = XR_PROVIDER_THREADS_ANY,
        .reentry = XR_PROVIDER_REENTRY_ALLOWED,
        .callbacks = XR_PROVIDER_CALLBACK_NONE,
        .refusal = XR_PROVIDER_REFUSAL_TRAP,
        .types = {XR_PROVIDER_TYPE_I64, XR_PROVIDER_TYPE_UNIT},
    };
}

static void test_wire_known_answer(void) {
    XrProviderLogicalContract contract = make_clock();
    static const uint8_t expected[] = {
        1, 0, 0, 0, 8, 0, 0, 0, 7, 0, 0, 0, 1, 0, 2, 0, 1, 1, 1, 1, 1, 1, 3, 1,
    };
    static const uint8_t digest[] = {
        0xbe, 0xf6, 0x49, 0x1c, 0xf0, 0xd7, 0x09, 0x6b, 0x74, 0xb5, 0x1f,
        0x39, 0x9d, 0x66, 0x62, 0xbf, 0xf7, 0x05, 0xe1, 0x27, 0x5a, 0x49,
        0xd4, 0x04, 0xfc, 0xcc, 0x76, 0xdb, 0x5f, 0x8c, 0xa0, 0x64,
    };
    uint8_t bytes[XR_PROVIDER_LOGICAL_MAX_ENCODED_BYTES];
    memset(bytes, 0xa5, sizeof(bytes));
    size_t size = 0u;
    CHECK(xr_provider_logical_contract_encode(&contract, bytes, sizeof(bytes), &size),
          "a complete logical contract encodes");
    CHECK(size == sizeof(expected) && memcmp(bytes, expected, sizeof(expected)) == 0,
          "canonical bytes agree with the independent wire known answer");
    CHECK(bytes[size] == 0xa5, "encoding writes only its declared payload");
    XrFingerprint fingerprint = {{0}};
    CHECK(xr_provider_logical_contract_fingerprint(&contract, &fingerprint) &&
              memcmp(fingerprint.bytes, digest, sizeof(digest)) == 0,
          "logical identity agrees with the independent SHA-256 known answer");
    XrProviderLogicalContract decoded = {0};
    CHECK(xr_provider_logical_contract_decode(expected, sizeof(expected), &decoded) &&
              memcmp(&contract, &decoded, sizeof(contract)) == 0,
          "wire decode preserves the complete logical facts");
}

static void test_semantic_identity_mutations(void) {
    XrProviderLogicalContract original = make_clock();
    XrFingerprint before = {{0}};
    CHECK(xr_provider_logical_contract_fingerprint(&original, &before), "clock identity exists");
    XrProviderLogicalContract changed[6];
    for (size_t index = 0u; index < 6u; ++index)
        changed[index] = original;
    changed[0].effects = XR_PROVIDER_EFFECT_READS_PROCESS;
    changed[1].threads = XR_PROVIDER_THREADS_INSTANCE_AFFINE;
    changed[2].reentry = XR_PROVIDER_REENTRY_FORBIDDEN;
    changed[3].callbacks = XR_PROVIDER_CALLBACK_SYNCHRONOUS;
    changed[4].platforms = XR_PROVIDER_PLATFORM_LINUX;
    changed[5].types[0] = XR_PROVIDER_TYPE_BOOL;
    for (size_t index = 0u; index < 6u; ++index) {
        XrFingerprint after = {{0}};
        CHECK(xr_provider_logical_contract_fingerprint(&changed[index], &after) &&
                  memcmp(before.bytes, after.bytes, sizeof(before.bytes)) != 0,
              "each observable logical fact changes identity without a physical ABI");
        CHECK(!xr_provider_logical_contract_equal(&original, &changed[index]) &&
                  !xr_provider_logical_contract_equal(&changed[index], &original),
              "logical admission rejects changed facts in either comparison direction");
    }
    original.effects |= XR_PROVIDER_EFFECT_MAY_ERROR;
    CHECK(!xr_provider_logical_contract_verify(&original),
          "a typed error effect requires its actual logical error type");
    original.types[1] = XR_PROVIDER_TYPE_I64;
    CHECK(xr_provider_logical_contract_verify(&original),
          "the explicit error type completes the typed error signature");
    original.effects &= ~XR_PROVIDER_EFFECT_MAY_ERROR;
    CHECK(!xr_provider_logical_contract_verify(&original),
          "an error type cannot be hidden behind a nothrow declaration");
}

static void test_absence_and_equality(void) {
    XrProviderLogicalContract absent = {0};
    XrProviderLogicalContract clock = make_clock();
    XrProviderLogicalContract copy = clock;
    CHECK(xr_provider_logical_contract_is_zero(&absent),
          "runtime foundations may have an explicitly absent language contract");
    CHECK(!xr_provider_logical_contract_is_zero(NULL) &&
              !xr_provider_logical_contract_is_zero(&clock),
          "neither null input nor a language contract represents absence");
    CHECK(!xr_provider_logical_contract_equal(&absent, &absent) &&
              !xr_provider_logical_contract_equal(NULL, &clock) &&
              !xr_provider_logical_contract_equal(&clock, NULL),
          "missing contracts never authorize an operation through equality");
    CHECK(xr_provider_logical_contract_equal(&clock, &copy),
          "independently stored complete logical facts compare equal");
    copy.resources[XR_PROVIDER_LOGICAL_MAX_RESOURCES - 1u].reserved[2] = 1u;
    CHECK(!xr_provider_logical_contract_equal(&copy, &copy),
          "identical malformed records cannot authorize an operation");
    absent.resources[XR_PROVIDER_LOGICAL_MAX_RESOURCES - 1u].path[3] = 1u;
    CHECK(!xr_provider_logical_contract_is_zero(&absent),
          "unused resource members cannot be hidden inside an absent contract");
}

static XrProviderLogicalContract make_pipe_open(void) {
    XrProviderLogicalContract contract = make_clock();
    const uint8_t types[] = {
        XR_PROVIDER_TYPE_OPTIONAL, XR_PROVIDER_TYPE_TUPLE, 2u,
        XR_PROVIDER_TYPE_I64,      XR_PROVIDER_TYPE_I64,   XR_PROVIDER_TYPE_UNIT,
    };
    contract.effects = XR_PROVIDER_EFFECT_IO;
    contract.type_byte_count = sizeof(types);
    memcpy(contract.types, types, sizeof(types));
    contract.resource_count = 2u;
    for (uint8_t index = 0u; index < 2u; ++index) {
        contract.resources[index] = (XrProviderLogicalResourceTransition) {
            .resource_id = {{0x23}},
            .source = XR_PROVIDER_RESOURCE_RESULT,
            .action = XR_PROVIDER_RESOURCE_ACQUIRE,
            .timing = XR_PROVIDER_RESOURCE_RESULT_PRESENT,
            .path_count = 2u,
            .path = {0u, index},
        };
    }
    return contract;
}

static void test_resource_paths_and_exits(void) {
    XrProviderLogicalContract open = make_pipe_open();
    CHECK(xr_provider_logical_contract_verify(&open),
          "optional pair resources are acquired only when the result is present");
    uint8_t bytes[XR_PROVIDER_LOGICAL_MAX_ENCODED_BYTES];
    size_t size = 0u;
    XrProviderLogicalContract decoded = {0};
    CHECK(xr_provider_logical_contract_encode(&open, bytes, sizeof(bytes), &size) &&
              xr_provider_logical_contract_decode(bytes, size, &decoded) &&
              memcmp(&open, &decoded, sizeof(open)) == 0,
          "resource identities, timing and exact result paths survive wire roundtrip");
    open.resources[1].path[1] = 2u;
    CHECK(!xr_provider_logical_contract_verify(&open), "resource paths must name an actual field");
    open = make_pipe_open();
    open.resources[1].path[1] = 0u;
    open.resources[1].resource_id.bytes[0] = 0x24;
    CHECK(!xr_provider_logical_contract_verify(&open),
          "one value cannot be published as two distinct resource owners");
    open = make_pipe_open();
    open.resources[0].timing = XR_PROVIDER_RESOURCE_CALL_ENTER;
    CHECK(!xr_provider_logical_contract_verify(&open),
          "an absent optional result cannot publish an acquired resource");
    XrProviderLogicalContract close = make_clock();
    close.effects = XR_PROVIDER_EFFECT_IO;
    close.parameter_count = 1u;
    close.parameter_modes[0] = XR_PROVIDER_MODE_IN;
    close.parameter_owners[0] = XR_PROVIDER_OWNER_TRIVIAL;
    close.type_byte_count = 3u;
    close.types[0] = XR_PROVIDER_TYPE_I64;
    close.types[1] = XR_PROVIDER_TYPE_BOOL;
    close.types[2] = XR_PROVIDER_TYPE_UNIT;
    close.resource_count = 1u;
    close.resources[0] = (XrProviderLogicalResourceTransition) {
        .resource_id = {{0x23}},
        .source = XR_PROVIDER_RESOURCE_PARAMETER,
        .action = XR_PROVIDER_RESOURCE_CONSUME,
        .timing = XR_PROVIDER_RESOURCE_CALL_ENTER,
    };
    CHECK(xr_provider_logical_contract_verify(&close),
          "close consumes the token on call entry independently of its boolean result");
    close.resources[0].timing = XR_PROVIDER_RESOURCE_RESULT_PRESENT;
    CHECK(!xr_provider_logical_contract_verify(&close),
          "token consumption cannot be conditional on an unrelated result shape");
}

static void test_hostile_wire_is_atomic(void) {
    XrProviderLogicalContract contract = make_pipe_open();
    uint8_t bytes[XR_PROVIDER_LOGICAL_MAX_ENCODED_BYTES];
    size_t size = 0u;
    CHECK(xr_provider_logical_contract_encode(&contract, bytes, sizeof(bytes), &size),
          "hostile-input seed encodes");
    XrProviderLogicalContract untouched;
    memset(&untouched, 0xa5, sizeof(untouched));
    for (size_t length = 0u; length < size; ++length) {
        XrProviderLogicalContract output = untouched;
        CHECK(!xr_provider_logical_contract_decode(bytes, length, &output) &&
                  memcmp(&output, &untouched, sizeof(output)) == 0,
              "every truncated payload is rejected without publication");
    }
    for (size_t offset = 0u; offset < size; ++offset) {
        uint8_t previous = bytes[offset];
        for (unsigned value = 0u; value <= 255u; ++value) {
            bytes[offset] = (uint8_t) value;
            XrProviderLogicalContract output = untouched;
            if (xr_provider_logical_contract_decode(bytes, size, &output)) {
                uint8_t encoded[XR_PROVIDER_LOGICAL_MAX_ENCODED_BYTES];
                size_t encoded_size = 0u;
                CHECK(xr_provider_logical_contract_encode(&output, encoded, sizeof(encoded),
                                                          &encoded_size) &&
                          encoded_size == size && memcmp(bytes, encoded, size) == 0,
                      "accepted hostile mutations have exactly one canonical representation");
            } else {
                CHECK(memcmp(&output, &untouched, sizeof(output)) == 0,
                      "invalid hostile mutations leave the destination unchanged");
            }
        }
        bytes[offset] = previous;
    }
    uint8_t before[XR_PROVIDER_LOGICAL_MAX_ENCODED_BYTES];
    memset(before, 0xa5, sizeof(before));
    memcpy(bytes, before, sizeof(bytes));
    size_t sentinel = 456u;
    CHECK(!xr_provider_logical_contract_encode(&contract, bytes, 1u, &sentinel) &&
              sentinel == 456u && memcmp(bytes, before, sizeof(bytes)) == 0,
          "insufficient output capacity publishes neither bytes nor length");
    contract.reserved[0] = 1u;
    XrFingerprint fingerprint;
    memset(&fingerprint, 0xa5, sizeof(fingerprint));
    XrFingerprint previous = fingerprint;
    CHECK(!xr_provider_logical_contract_fingerprint(&contract, &fingerprint) &&
              memcmp(&fingerprint, &previous, sizeof(fingerprint)) == 0,
          "reserved semantic data cannot obtain a logical fingerprint");
}

static void test_owned_resource_type(void) {
    XrProviderLogicalContract contract = make_clock();
    contract.effects = XR_PROVIDER_EFFECT_IO;
    contract.result_owner = XR_PROVIDER_OWNER_OWNED;
    contract.type_byte_count = 19u;
    memset(contract.types, 0, sizeof(contract.types));
    contract.types[0] = XR_PROVIDER_TYPE_OPTIONAL;
    contract.types[1] = XR_PROVIDER_TYPE_RESOURCE;
    for (uint8_t index = 0u; index < XR_STABLE_ID_BYTES; ++index)
        contract.types[2u + index] = (uint8_t) (index + 1u);
    contract.types[18] = XR_PROVIDER_TYPE_UNIT;
    contract.resource_count = 1u;
    contract.resources[0] = (XrProviderLogicalResourceTransition) {
        .source = XR_PROVIDER_RESOURCE_RESULT, .action = XR_PROVIDER_RESOURCE_ACQUIRE,
        .timing = XR_PROVIDER_RESOURCE_RESULT_PRESENT, .path_count = 1u,
    };
    memcpy(contract.resources[0].resource_id.bytes, contract.types + 2u, XR_STABLE_ID_BYTES);
    static const uint8_t expected[] = {1,0,0,0,64,0,0,0,7,0,0,0,1,0,19,1,4,1,1,1,1,1,6,7,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,1,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,2,0,1,2,1,0};
    static const uint8_t digest[] = {0x34,0x21,0x67,0xa2,0xb0,0x61,0xe7,0x66,0x68,0xaa,0xe8,0xa0,0x3e,0x55,0x05,0x9b,0x8e,0x7c,0x21,0x9b,0x95,0x01,0xf8,0xb0,0x30,0xbd,0x31,0xc3,0x4f,0xc0,0x13,0x3f};
    uint8_t bytes[XR_PROVIDER_LOGICAL_MAX_ENCODED_BYTES];
    size_t size = 0u;
    XrFingerprint fingerprint;
    CHECK(xr_provider_logical_contract_encode(&contract, bytes, sizeof(bytes), &size) &&
          size == sizeof(expected) && memcmp(bytes, expected, size) == 0,
          "resource identity and ownership have an independent exact wire");
    CHECK(xr_provider_logical_contract_fingerprint(&contract, &fingerprint) &&
          memcmp(fingerprint.bytes, digest, sizeof(digest)) == 0,
          "resource fingerprint covers exact identity without target layout");
    XrProviderLogicalContract decoded = {0};
    CHECK(xr_provider_logical_contract_decode(expected, sizeof(expected), &decoded) &&
          xr_provider_logical_contract_equal(&contract, &decoded), "resource wire roundtrip");
    XrStableId id = {{0x55}}, untouched = id;
    XrProviderLogicalTypeView leaf = {contract.types + 1u, 17u};
    CHECK(xr_provider_logical_resource_type_id(leaf, &id) &&
          memcmp(id.bytes, contract.resources[0].resource_id.bytes, XR_STABLE_ID_BYTES) == 0, "resource leaf identity extraction");
    leaf.size = 16u;
    id = untouched;
    CHECK(!xr_provider_logical_resource_type_id(leaf, &id) && memcmp(id.bytes, untouched.bytes, XR_STABLE_ID_BYTES) == 0,
          "truncated resource leaves the output unchanged");
    for (size_t length = 0u; length < sizeof(expected); ++length) {
        decoded = make_clock();
        XrProviderLogicalContract before = decoded;
        CHECK(!xr_provider_logical_contract_decode(expected, length, &decoded) &&
              memcmp(&before, &decoded, sizeof(before)) == 0, "truncated resource wire is atomic");
    }
    XrProviderLogicalContract changed = contract;
    changed.result_owner = XR_PROVIDER_OWNER_TRIVIAL;
    CHECK(!xr_provider_logical_contract_verify(&changed), "resource payload is never trivial ownership");
    changed = contract;
    memset(changed.types + 2u, 0, XR_STABLE_ID_BYTES);
    CHECK(!xr_provider_logical_contract_verify(&changed), "zero resource identity is invalid");
    changed = contract;
    changed.resources[0].resource_id.bytes[0] ^= 0x80u;
    CHECK(!xr_provider_logical_contract_verify(&changed), "transition must match the resource leaf identity");
    changed.types[2] ^= 0x80u;
    CHECK(xr_provider_logical_contract_verify(&changed) &&
          !xr_provider_logical_contract_equal(&contract, &changed), "different resource type is a distinct contract");
    changed = contract;
    changed.types[1] = 8u;
    CHECK(!xr_provider_logical_contract_verify(&changed), "unknown type tokens remain rejected");
    changed = contract;
    changed.parameter_count = 1u;
    changed.parameter_modes[0] = XR_PROVIDER_MODE_IN;
    changed.parameter_owners[0] = XR_PROVIDER_OWNER_BORROWED;
    changed.result_owner = XR_PROVIDER_OWNER_TRIVIAL;
    memmove(changed.types, contract.types + 1u, 17u);
    changed.types[17] = XR_PROVIDER_TYPE_BOOL;
    changed.types[18] = XR_PROVIDER_TYPE_UNIT;
    changed.resources[0].source = XR_PROVIDER_RESOURCE_PARAMETER;
    changed.resources[0].action = XR_PROVIDER_RESOURCE_CONSUME;
    changed.resources[0].timing = XR_PROVIDER_RESOURCE_CALL_ENTER;
    changed.resources[0].path_count = 0u;
    CHECK(xr_provider_logical_contract_verify(&changed),
          "closing the external resource may borrow its still-live managed handle");
    changed.parameter_owners[0] = XR_PROVIDER_OWNER_TRIVIAL;
    CHECK(!xr_provider_logical_contract_verify(&changed), "managed input cannot masquerade as a scalar");
    changed = contract;
    memmove(changed.types + 4u, contract.types + 1u, 17u);
    changed.types[1] = XR_PROVIDER_TYPE_TUPLE;
    changed.types[2] = 2u;
    changed.types[3] = XR_PROVIDER_TYPE_I64;
    changed.types[21] = XR_PROVIDER_TYPE_UNIT;
    changed.type_byte_count = 22u;
    changed.resources[0].path_count = 2u;
    changed.resources[0].path[1] = 1u;
    CHECK(xr_provider_logical_contract_verify(&changed), "nested resource paths cross exact tuple children");
    changed.resources[0].path[1] = 2u;
    CHECK(!xr_provider_logical_contract_verify(&changed), "nested resource paths cannot escape tuple arity");
    for (size_t offset = 0u; offset < sizeof(expected); ++offset) {
        memcpy(bytes, expected, sizeof(expected));
        bytes[offset] ^= 0x80u;
        decoded = make_clock();
        XrProviderLogicalContract before = decoded;
        if (!xr_provider_logical_contract_decode(bytes, sizeof(expected), &decoded))
            CHECK(memcmp(&before, &decoded, sizeof(before)) == 0, "hostile resource mutations publish no partial contract");
        else {
            uint8_t again[XR_PROVIDER_LOGICAL_MAX_ENCODED_BYTES];
            size_t again_size = 0u;
            CHECK(xr_provider_logical_contract_encode(&decoded, again, sizeof(again), &again_size) &&
                  again_size == sizeof(expected) && memcmp(bytes, again, again_size) == 0,
                  "valid mutated resource contracts have exact canonical bytes");
        }
    }

}

int main(void) {
    test_wire_known_answer();
    test_owned_resource_type();
    test_semantic_identity_mutations();
    test_absence_and_equality();
    test_resource_paths_and_exits();
    test_hostile_wire_is_atomic();
    if (failures != 0)
        return 1;
    puts("provider logical contract tests passed");
    return 0;
}
