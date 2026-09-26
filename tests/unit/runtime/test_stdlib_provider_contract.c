/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_stdlib_provider_contract.c - Generated logical contract cross-checks
 */

#include "runtime/abi/xr_stdlib_provider_contract.h"
#include "runtime/abi/xr_stdlib_provider_projection.h"
#include "runtime/abi/xr_runtime_target_authority.h"
#include "plan/semantic/xr_semantic_ids.h"
#include "stdlib/xstdlib_metadata.h"

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

static void resource_declaration_identity(void) {
    const XrStdlibNativeClassDefEntry *entry =
        xr_stdlib_metadata_unique_native_class_span("net", 3u, "__NetConnStorage", 16u);
    CHECK(entry != NULL, "resource declaration resolves by exact module and name");
    if (!entry) return;
    const uint8_t expected[16] = {0x0b, 0x55, 0x1a, 0x7a, 0x09, 0x78, 0x02, 0x97, 0x3f, 0x0b, 0x08, 0x7b, 0xd7, 0x96, 0x56, 0xc3};
    XrStableId identity = {{0}}, altered = {{0}};
    CHECK(xr_stdlib_metadata_resource_identity(entry, &identity), "resource identity exists");
    CHECK(memcmp(identity.bytes, expected, sizeof(expected)) == 0, "independent resource identity");
    XrStdlibNativeClassDefEntry mutation = *entry;
    mutation.native_body_expr = "different_body()";
    mutation.core_slot = "differentSlot";
    mutation.flags = "differentFlags";
    mutation.source_wrapper = "DifferentWrapper";
    mutation.source_storage_field = "differentField";
    mutation.builtin_kind = "differentBuiltin";
    mutation.super_slot = "differentBase";
    CHECK(xr_stdlib_metadata_resource_identity(&mutation, &altered) &&
          memcmp(identity.bytes, altered.bytes, sizeof(identity.bytes)) == 0,
          "physical representation does not alter logical identity");
    mutation.name = "__NetListenerStorage";
    CHECK(xr_stdlib_metadata_resource_identity(&mutation, &altered) &&
          memcmp(identity.bytes, altered.bytes, sizeof(identity.bytes)) != 0,
          "different declarations have different resource identities");
    mutation = *entry;
    mutation.module = "http";
    CHECK(xr_stdlib_metadata_resource_identity(&mutation, &altered) &&
          memcmp(identity.bytes, altered.bytes, sizeof(identity.bytes)) != 0,
          "module belongs to resource identity");
    altered = identity;
    mutation.module = ".net";
    CHECK(!xr_stdlib_metadata_resource_identity(&mutation, &altered) &&
          memcmp(identity.bytes, altered.bytes, sizeof(identity.bytes)) == 0,
          "invalid identity leaves output untouched");
    CHECK(!xr_stdlib_metadata_resource_identity(NULL, &altered), "missing declaration rejected");
    CHECK(!xr_stdlib_metadata_resource_identity(entry, NULL), "missing output rejected");
}

static void generated_contracts(void) {
    CHECK(xr_stdlib_provider_count() == 12u, "scalar, byte-storage and entropy declarations are generated");
    CHECK(xr_stdlib_provider_at(xr_stdlib_provider_count()) == NULL, "index bounds are checked");
    const XrStdlibProviderDescriptor *previous = NULL;
    unsigned resources = 0u;
    for (size_t index = 0u; index < xr_stdlib_provider_count(); ++index) {
        const XrStdlibProviderDescriptor *row = xr_stdlib_provider_at(index);
        XrStableId id;
        XrFingerprint digest;
        CHECK(xr_stable_id_from_key(row->contract_key, &id, &digest) &&
                  xr_stable_id_equal(id, row->contract_id),
              "contract ID matches the canonical key");
        CHECK(xr_stable_id_from_key(row->operation_key, &id, &digest) &&
                  xr_stable_id_equal(id, row->operation_id),
              "operation ID matches the canonical key");
        CHECK(xr_stdlib_provider_find(row->contract_id, row->operation_id) == row,
              "lookup uses complete identity");
        if (previous) {
            int order = xr_stable_id_compare(previous->contract_id, row->contract_id);
            CHECK(order < 0 || (order == 0 && xr_stable_id_compare(previous->operation_id,
                                                                   row->operation_id) < 0),
                  "descriptor order is canonical and unique");
        }
        previous = row;
        XrProviderLogicalContract logical;
        if (!xr_stdlib_provider_logical(row, &logical)) {
            CHECK(false, "C validates each generated logical contract and fingerprint");
            continue;
        }
        uint8_t encoded[XR_PROVIDER_LOGICAL_MAX_ENCODED_BYTES];
        size_t size = 0u;
        CHECK(xr_provider_logical_contract_encode(&logical, encoded, sizeof(encoded), &size) &&
                  size == row->logical_size && memcmp(encoded, row->logical_bytes, size) == 0,
              "C encoding independently reproduces the generated bytes");
        CHECK(logical.threads == XR_PROVIDER_THREADS_ANY &&
                  logical.reentry == (strcmp(row->symbol, "crypto.__fillRandomBytes") == 0
                                      ? XR_PROVIDER_REENTRY_FORBIDDEN : XR_PROVIDER_REENTRY_ALLOWED) &&
                  logical.callbacks == XR_PROVIDER_CALLBACK_NONE &&
                  logical.refusal == XR_PROVIDER_REFUSAL_TRAP,
              "execution constraints are explicit");
        CHECK((logical.effects & (XR_PROVIDER_EFFECT_MAY_ERROR | XR_PROVIDER_EFFECT_MAY_PANIC |
                                  XR_PROVIDER_EFFECT_MAY_SUSPEND)) == 0u,
              "admitted leaves have no language error, panic or suspension exit");
        resources += logical.resource_count;
        for (uint8_t resource = 0u; resource < logical.resource_count; ++resource) {
            CHECK(xr_stable_id_from_key("xray.runtime.resource.v1/pipe-endpoint", &id, &digest) &&
                      xr_stable_id_equal(id, logical.resources[resource].resource_id),
                  "resource identity uses canonical framing");
        }
    }
    CHECK(resources == 3u, "pipe open acquires two tokens and close consumes one");
    XrStableId absent = {{0}};
    CHECK(xr_stdlib_provider_find(absent, absent) == NULL, "unknown identity cannot bind");
}

static void byte_storage_logical_contracts(void) {
    const char *symbols[] = {"mem.__alloc", "mem.__allocZeroed", "mem.__allocAligned",
                             "mem.__bufferLength"};
    const XrStdlibNativeClassDefEntry *storage =
        xr_stdlib_metadata_unique_native_class_span("mem", 3u, "__BufferStorage", 15u);
    XrStableId identity = {{0}};
    CHECK(storage && xr_stdlib_metadata_resource_identity(storage, &identity),
          "storage signatures resolve the existing nominal identity");
    for (size_t operation = 0u; operation < 4u; ++operation) {
        const XrStdlibProviderDescriptor *descriptor = NULL;
        for (size_t index = 0u; index < xr_stdlib_provider_count(); ++index) {
            const XrStdlibProviderDescriptor *candidate = xr_stdlib_provider_at(index);
            if (strcmp(candidate->symbol, symbols[operation]) == 0)
                descriptor = candidate;
        }
        XrProviderLogicalContract logical;
        CHECK(descriptor && xr_stdlib_provider_logical(descriptor, &logical),
              "each storage operation has a valid generated declaration");
        if (!descriptor || !xr_stdlib_provider_logical(descriptor, &logical))
            continue;
        bool length = operation == 3u;
        uint8_t parameters = operation == 2u ? 2u : 1u;
        uint8_t expected[20] = {0};
        size_t offset = 0u;
        if (!length)
            for (uint8_t parameter = 0u; parameter < parameters; ++parameter)
                expected[offset++] = XR_PROVIDER_TYPE_I64;
        expected[offset++] = XR_PROVIDER_TYPE_RESOURCE;
        memcpy(expected + offset, identity.bytes, sizeof(identity.bytes));
        offset += sizeof(identity.bytes);
        if (length)
            expected[offset++] = XR_PROVIDER_TYPE_I64;
        expected[offset++] = XR_PROVIDER_TYPE_UNIT;
        CHECK(logical.type_byte_count == offset && memcmp(logical.types, expected, offset) == 0,
              "resource type, scalar arguments and unit error match independent expectations");
        CHECK(logical.parameter_count == parameters &&
                  logical.result_owner == (length ? XR_PROVIDER_OWNER_TRIVIAL : XR_PROVIDER_OWNER_OWNED),
              "allocation transfers storage while length returns a trivial value");
        for (uint8_t parameter = 0u; parameter < parameters; ++parameter)
            CHECK(logical.parameter_modes[parameter] == XR_PROVIDER_MODE_IN &&
                      logical.parameter_owners[parameter] ==
                          (length ? XR_PROVIDER_OWNER_BORROWED : XR_PROVIDER_OWNER_TRIVIAL),
                  "storage input borrows and scalar inputs remain trivial");
        CHECK(logical.effects == (length ? 0u : XR_PROVIDER_EFFECT_MANAGED_ALLOCATION) &&
                  logical.resource_count == 0u,
              "managed storage ownership does not invent an external resource token transition");
    }
}

static void corrupted_records_are_not_published(void) {
    const XrStdlibProviderDescriptor *row = xr_stdlib_provider_at(0u);
    if (!row) {
        CHECK(false, "the generated registry is nonempty");
        return;
    }
    XrStdlibProviderDescriptor changed = *row;
    XrProviderLogicalContract output;
    memset(&output, 0x5a, sizeof(output));
    XrProviderLogicalContract before = output;
    changed.logical_fingerprint.bytes[0] ^= 1u;
    CHECK(!xr_stdlib_provider_logical(&changed, &output) &&
              memcmp(&before, &output, sizeof(output)) == 0,
          "fingerprint mismatch leaves the output unchanged");
    changed = *row;
    --changed.logical_size;
    CHECK(!xr_stdlib_provider_logical(&changed, &output) &&
              memcmp(&before, &output, sizeof(output)) == 0,
          "truncated records leave the output unchanged");
    CHECK(!xr_stdlib_provider_logical(NULL, &output), "a missing record is not a contract");
    CHECK(!xr_stdlib_provider_logical(row, NULL), "output storage is required");
    XrStableId wrong_contract = row->contract_id;
    wrong_contract.bytes[0] ^= 1u;
    CHECK(xr_stdlib_provider_find(wrong_contract, row->operation_id) == NULL,
          "operation identity cannot authorize another contract");
}

static void native_authority_consumes_generated_contracts(void) {
    XrRuntimeTargetAuthority authority;
    if (xr_runtime_target_authority_native_hosted(&authority) != XR_RUNTIME_ABI_OK) {
        CHECK(false, "the native authority accepts every generated provider");
        return;
    }
    CHECK(authority.provider_count == 7u, "the registry joins foundations, output and declared providers");
    for (size_t index = 0u; index < xr_stdlib_provider_count(); ++index) {
        const XrStdlibProviderDescriptor *descriptor = xr_stdlib_provider_at(index);
        XrTargetProviderOperationContract projected;
        CHECK(xr_stdlib_provider_project(descriptor, &authority.machine, &projected) ==
                  XR_STDLIB_PROVIDER_PROJECTION_OK,
              "explicit host adapter has a target projection");
        unsigned matches = 0u;
        for (size_t provider = 0u; provider < authority.provider_count; ++provider) {
            const XrTargetProviderContract *contract = &authority.providers[provider];
            if (!xr_stable_id_equal(contract->contract_id, descriptor->contract_id))
                continue;
            for (uint16_t operation = 0u; operation < contract->operation_count; ++operation) {
                const XrTargetProviderOperationContract *row = &contract->operations[operation];
                if (!xr_stable_id_equal(row->stable_id, descriptor->operation_id))
                    continue;
                ++matches;
                XrFingerprint actual, expected;
                CHECK(xr_target_provider_call_abi_fingerprint(&row->call_abi, &actual) ==
                              XR_RUNTIME_ABI_OK &&
                          xr_target_provider_call_abi_fingerprint(&projected.call_abi, &expected) ==
                              XR_RUNTIME_ABI_OK &&
                          xr_fingerprint_equal(actual, expected),
                      "the native authority consumes the generated adapter ABI");
                CHECK(row->effect_flags == projected.effect_flags &&
                          row->lifetime_flags == projected.lifetime_flags &&
                          row->failure_flags == projected.failure_flags,
                      "effect, lifetime and failure projection are retained");
            }
        }
        CHECK(matches == 1u, "the generated operation has exactly one authority entry");
    }
    const XrStdlibProviderDescriptor *descriptor = xr_stdlib_provider_at(0u);
    XrTargetProviderOperationContract output;
    memset(&output, 0x5a, sizeof(output));
    XrTargetProviderOperationContract before = output;
    authority.machine.runtime_profile = XR_TARGET_RUNTIME_PROFILE_FREESTANDING;
    CHECK(xr_stdlib_provider_project(descriptor, &authority.machine, &output) ==
                  XR_STDLIB_PROVIDER_PROJECTION_UNAVAILABLE &&
              memcmp(&before, &output, sizeof(output)) == 0,
          "an unsupported runtime profile cannot publish a provider ABI");
    authority.machine.runtime_profile = XR_TARGET_RUNTIME_PROFILE_HOSTED;
    authority.machine.operating_system = UINT16_MAX;
    CHECK(xr_stdlib_provider_project(descriptor, &authority.machine, &output) ==
              XR_STDLIB_PROVIDER_PROJECTION_UNAVAILABLE,
          "an undeclared platform is unavailable");
    authority.machine.operating_system = XR_TARGET_OS_LINUX;
    authority.machine.data_layout.pointer.size = 3u;
    CHECK(xr_stdlib_provider_project(descriptor, &authority.machine, &output) ==
              XR_STDLIB_PROVIDER_PROJECTION_INVALID,
          "malformed target layout is distinct from unavailable support");
    XrStdlibProviderDescriptor authored = *descriptor;
    CHECK(xr_stdlib_provider_project(&authored, &authority.machine, &output) ==
              XR_STDLIB_PROVIDER_PROJECTION_INVALID,
          "caller-authored descriptors do not replace generated authority");
}

#include "xr_byte_storage_provider_checks.inc.c"

int main(void) {
    resource_declaration_identity();
    generated_contracts();
    byte_storage_logical_contracts();
    corrupted_records_are_not_published();
    native_authority_consumes_generated_contracts();
    byte_storage_physical_checks();
    return failures ? 1 : 0;
}
