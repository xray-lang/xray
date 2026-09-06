/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * target_profile_test_fixture.c - Structured TargetProfile test authority
 */

#include "target_profile_test_fixture.h"
#include "../../../src/plan/semantic/xr_semantic_ids.h"
#include "../../../src/runtime/abi/xr_builtin_provider_contract.h"
#include <stdio.h>
#include <string.h>

/* Reuse the frozen runtime/provider schema fixture instead of maintaining a
 * second schema-shaped initializer. Its test entry point is private here. */
#define main xr_test_runtime_abi_contract_fixture_main
#include "../runtime/test_runtime_abi_contract.c"
#undef main

static XrRuntimeObjectHeaderMaterializationFacts make_header_facts(
    uint8_t endian) {
    return (XrRuntimeObjectHeaderMaterializationFacts) {
        .schema_version = XR_RUNTIME_OBJECT_HEADER_FACTS_SCHEMA_VERSION,
        .header_size = XR_RUNTIME_OBJECT_HEADER_SIZE,
        .header_alignment = XR_RUNTIME_OBJECT_HEADER_ALIGNMENT,
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

static void retarget_call_abi(XrTargetProviderCallAbiContract *abi,
                              uint8_t pointer_width, uint8_t endian) {
    abi->target_endian = endian;
    abi->pointer_width = pointer_width;
    abi->pointer_alignment = pointer_width;
    XrTargetProviderCallSlotAbi *slots = &abi->result;
    for (size_t i = 0; i <= abi->parameter_count; i++) {
        XrTargetProviderCallSlotAbi *slot =
            i == 0 ? slots : &abi->parameters[i - 1];
        if (slot->value_kind == XR_TARGET_PROVIDER_CALL_VALUE_DATA_ADDRESS ||
            slot->value_kind == XR_TARGET_PROVIDER_CALL_VALUE_CODE_ADDRESS ||
            slot->value_kind == XR_TARGET_PROVIDER_CALL_VALUE_UNSIGNED_INTEGER) {
            slot->width = pointer_width;
            slot->alignment = pointer_width;
        }
    }
}

static void retarget_providers(XrTargetProviderContract providers[2],
                               uint8_t pointer_width, uint8_t endian,
                               uint8_t runtime_profile) {
    uint32_t availability =
        runtime_profile == XR_TARGET_RUNTIME_PROFILE_FREESTANDING
            ? XR_TARGET_PROVIDER_AVAILABLE_FREESTANDING
            : XR_TARGET_PROVIDER_AVAILABLE_HOSTED;
    for (size_t i = 0; i < 2; i++) {
        providers[i].flags = availability;
        providers[i].runtime_profile = runtime_profile;
        for (size_t operation = 0; operation < providers[i].operation_count;
             operation++)
            retarget_call_abi(&providers[i].operations[operation].call_abi,
                              pointer_width, endian);
    }
}

bool xr_test_target_profile_fixture_init(XrTestTargetProfileFixture *fixture,
                                         bool ilp32, uint8_t runtime_profile) {
    if (!fixture)
        return false;
    memset(fixture, 0, sizeof(*fixture));
    XrTargetMachineFacts *machine = &fixture->input.machine;
    machine->architecture = ilp32 ? XR_TARGET_ARCH_WASM32
                                  : XR_TARGET_ARCH_X86_64;
    machine->operating_system = ilp32 ? XR_TARGET_OS_WASI : XR_TARGET_OS_WINDOWS;
    machine->environment = ilp32 ? XR_TARGET_ENV_WASI : XR_TARGET_ENV_MSVC;
    machine->native_abi = ilp32 ? XR_TARGET_ABI_WASM
                                : XR_TARGET_ABI_WIN64_X86_64;
    machine->runtime_profile = runtime_profile;
    if (!(ilp32 ? xr_target_data_layout_init_ilp32(&machine->data_layout)
                : xr_target_data_layout_init_lp64(&machine->data_layout)))
        return false;
    machine->atomic_width_mask = XR_TARGET_ATOMIC_WIDTH_8 |
                                 XR_TARGET_ATOMIC_WIDTH_16 |
                                 XR_TARGET_ATOMIC_WIDTH_32 |
                                 XR_TARGET_ATOMIC_WIDTH_64;
    machine->atomic_order_mask = XR_TARGET_ATOMIC_RELAXED |
                                 XR_TARGET_ATOMIC_ACQUIRE |
                                 XR_TARGET_ATOMIC_RELEASE |
                                 XR_TARGET_ATOMIC_ACQ_REL |
                                 XR_TARGET_ATOMIC_SEQ_CST;
    machine->float_feature_mask = XR_TARGET_FLOAT_IEEE754 |
                                  XR_TARGET_FLOAT_STRICT;
    machine->vector_feature_mask = ilp32 ? XR_TARGET_VECTOR_WASM128
                                         : XR_TARGET_VECTOR_SSE2;
    machine->maximum_vector_bits = 128;

    uint8_t pointer_width = (uint8_t) machine->data_layout.pointer.size;
    uint8_t endian = (uint8_t) machine->data_layout.endian;
    fixture->object_header_materialization = make_header_facts(endian);
    if (xr_runtime_string_object_contract_build(&fixture->string_contract) !=
        XR_RUNTIME_ABI_OK)
        return false;
    fixture->runtime_abi = make_runtime_abi();
    fixture->runtime_abi.pointer_width = pointer_width;
    fixture->runtime_abi.target_endian = endian;
    fixture->runtime_abi.dynamic_value.target_endian = endian;
    fixture->runtime_abi.dynamic_value.object_reference_width = pointer_width;
    fixture->runtime_abi.extent_provider_callback.operand_count_width =
        pointer_width;
    if (xr_runtime_object_header_abi_materialize(
            &fixture->object_header_materialization,
            &fixture->runtime_abi.object_header) != XR_RUNTIME_ABI_OK)
        return false;
    make_providers(fixture->providers);
    retarget_providers(fixture->providers, pointer_width, endian,
                       runtime_profile);
    fixture->input.runtime_abi = &fixture->runtime_abi;
    fixture->input.object_header_materialization =
        &fixture->object_header_materialization;
    fixture->input.string_contract = &fixture->string_contract;
    fixture->input.providers = fixture->providers;
    fixture->input.provider_count = 2;
    return true;
}

XrTargetProfile *xr_test_target_profile_build(bool ilp32,
                                              uint8_t runtime_profile) {
    XrTestTargetProfileFixture fixture;
    if (!xr_test_target_profile_fixture_init(&fixture, ilp32, runtime_profile))
        return NULL;
    XrTargetProfile *profile = NULL;
    char error[512] = {0};
    if (!xr_target_profile_build(&fixture.input, &profile, error, sizeof(error))) {
        fprintf(stderr, "structured target profile fixture failed: %s\n", error);
        return NULL;
    }
    return profile;
}

XrTargetProfile *xr_test_target_profile_build_with_scalar_clock(
    bool ilp32, uint8_t runtime_profile, uint8_t scalar_value_kind) {
    XrTestTargetProfileFixture fixture;
    if (!xr_test_target_profile_fixture_init(&fixture, ilp32, runtime_profile))
        return NULL;
    if (scalar_value_kind != XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER &&
        scalar_value_kind != XR_TARGET_PROVIDER_CALL_VALUE_UNSIGNED_INTEGER)
        return NULL;

    XrTargetProviderContract providers[3] = {0};
    memcpy(providers, fixture.providers, sizeof(fixture.providers));
    uint32_t availability =
        runtime_profile == XR_TARGET_RUNTIME_PROFILE_FREESTANDING
            ? XR_TARGET_PROVIDER_AVAILABLE_FREESTANDING
            : XR_TARGET_PROVIDER_AVAILABLE_HOSTED;
    XrTargetProviderCallSlotAbi scalar =
        make_call_slot(scalar_value_kind, 8u, 8u,
                       XR_TARGET_PROVIDER_CALL_OWNERSHIP_NONE, 0u);
    XrTargetProviderCallAbiContract scalar_abi = make_call_abi(scalar, &scalar, 1u);
    retarget_call_abi(&scalar_abi,
                      (uint8_t) fixture.input.machine.data_layout.pointer.size,
                      (uint8_t) fixture.input.machine.data_layout.endian);
    providers[2] = (XrTargetProviderContract) {
        .schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .contract_id = make_id(202u),
        .abi_schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .flags = availability,
        .operation_count = 1u,
        .runtime_profile = runtime_profile,
        .provider_kind = XR_TARGET_PROVIDER_CLOCK,
    };
    providers[2].operations[0] =
        make_operation(21u, scalar_abi, 0u, 0u, 0u);
    fixture.input.providers = providers;
    fixture.input.provider_count = 3u;

    XrTargetProfile *profile = NULL;
    char error[512] = {0};
    if (!xr_target_profile_build(&fixture.input, &profile, error, sizeof(error))) {
        fprintf(stderr, "scalar clock target profile fixture failed: %s\n", error);
        return NULL;
    }
    return profile;
}

XrTargetProfile *xr_test_target_profile_build_with_nullary_clock(
    bool ilp32, uint8_t runtime_profile, uint8_t scalar_value_kind) {
    XrTestTargetProfileFixture fixture;
    if (!xr_test_target_profile_fixture_init(&fixture, ilp32, runtime_profile))
        return NULL;
    if (scalar_value_kind != XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER &&
        scalar_value_kind != XR_TARGET_PROVIDER_CALL_VALUE_UNSIGNED_INTEGER)
        return NULL;

    XrTargetProviderContract providers[3] = {0};
    memcpy(providers, fixture.providers, sizeof(fixture.providers));
    uint32_t availability =
        runtime_profile == XR_TARGET_RUNTIME_PROFILE_FREESTANDING
            ? XR_TARGET_PROVIDER_AVAILABLE_FREESTANDING
            : XR_TARGET_PROVIDER_AVAILABLE_HOSTED;
    XrTargetProviderCallSlotAbi scalar =
        make_call_slot(scalar_value_kind, 8u, 8u,
                       XR_TARGET_PROVIDER_CALL_OWNERSHIP_NONE, 0u);
    XrTargetProviderCallAbiContract scalar_abi = make_call_abi(scalar, NULL, 0u);
    retarget_call_abi(&scalar_abi,
                      (uint8_t) fixture.input.machine.data_layout.pointer.size,
                      (uint8_t) fixture.input.machine.data_layout.endian);
    providers[2] = (XrTargetProviderContract) {
        .schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .contract_id = make_id(203u),
        .abi_schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .flags = availability,
        .operation_count = 1u,
        .runtime_profile = runtime_profile,
        .provider_kind = XR_TARGET_PROVIDER_CLOCK,
    };
    providers[2].operations[0] = make_operation(22u, scalar_abi, 0u, 0u, 0u);
    fixture.input.providers = providers;
    fixture.input.provider_count = 3u;

    XrTargetProfile *profile = NULL;
    char error[512] = {0};
    if (!xr_target_profile_build(&fixture.input, &profile, error, sizeof(error))) {
        fprintf(stderr, "nullary clock target profile fixture failed: %s\n", error);
        return NULL;
    }
    return profile;
}

XrTargetProfile *xr_test_target_profile_build_with_output(bool ilp32,
                                                          uint8_t runtime_profile) {
    XrTestTargetProfileFixture fixture;
    if (!xr_test_target_profile_fixture_init(&fixture, ilp32, runtime_profile))
        return NULL;

    XrTargetProviderContract providers[3] = {0};
    memcpy(providers, fixture.providers, sizeof(fixture.providers));
    uint8_t pointer_width = (uint8_t) fixture.input.machine.data_layout.pointer.size;
    uint8_t pointer_alignment = (uint8_t) fixture.input.machine.data_layout.pointer.align;
    XrTargetProviderCallSlotAbi status = make_call_slot(
        XR_TARGET_PROVIDER_CALL_VALUE_UNSIGNED_INTEGER, 1u, 1u,
        XR_TARGET_PROVIDER_CALL_OWNERSHIP_NONE, 0u);
    XrTargetProviderCallSlotAbi parameters[] = {
        make_call_slot(XR_TARGET_PROVIDER_CALL_VALUE_DATA_ADDRESS, pointer_width,
                       pointer_alignment, XR_TARGET_PROVIDER_CALL_OWNERSHIP_BORROWED,
                       XR_TARGET_PROVIDER_CALL_SLOT_NULLABLE),
        make_call_slot(XR_TARGET_PROVIDER_CALL_VALUE_DATA_ADDRESS, pointer_width,
                       pointer_alignment, XR_TARGET_PROVIDER_CALL_OWNERSHIP_BORROWED,
                       XR_TARGET_PROVIDER_CALL_SLOT_CONST_POINTEE),
        make_call_slot(XR_TARGET_PROVIDER_CALL_VALUE_UNSIGNED_INTEGER, pointer_width,
                       pointer_alignment, XR_TARGET_PROVIDER_CALL_OWNERSHIP_NONE, 0u),
    };
    XrTargetProviderCallAbiContract call_abi = make_call_abi(status, parameters, 3u);
    call_abi.target_endian = (uint8_t) fixture.input.machine.data_layout.endian;
    call_abi.pointer_width = pointer_width;
    call_abi.pointer_alignment = pointer_alignment;
    uint32_t availability =
        runtime_profile == XR_TARGET_RUNTIME_PROFILE_FREESTANDING
            ? XR_TARGET_PROVIDER_AVAILABLE_FREESTANDING
            : XR_TARGET_PROVIDER_AVAILABLE_HOSTED;
    providers[2] = (XrTargetProviderContract) {
        .schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .abi_schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .flags = availability,
        .operation_count = 1u,
        .runtime_profile = runtime_profile,
        .provider_kind = XR_TARGET_PROVIDER_IO,
    };
    providers[2].operations[0] = make_operation(
        0u, call_abi, XR_TARGET_PROVIDER_EFFECT_IO,
        XR_TARGET_PROVIDER_LIFETIME_BORROWS,
        XR_TARGET_PROVIDER_FAILURE_RETURNS_STATUS);
    XrFingerprint key_digest;
    if (!xr_stable_id_from_key(XR_PROVIDER_IO_CONTRACT_KEY,
                               &providers[2].contract_id, &key_digest) ||
        !xr_stable_id_from_key(XR_PROVIDER_IO_OUTPUT_WRITE_OPERATION_KEY,
                               &providers[2].operations[0].stable_id, &key_digest))
        return NULL;
    fixture.input.providers = providers;
    fixture.input.provider_count = 3u;
    XrTargetProfile *profile = NULL;
    char error[512] = {0};
    if (!xr_target_profile_build(&fixture.input, &profile, error, sizeof(error))) {
        fprintf(stderr, "output target profile fixture failed: %s\n", error);
        return NULL;
    }
    return profile;
}

XrTargetProfile *xr_test_target_profile_build_with_pipe(bool ilp32,
                                                        uint8_t runtime_profile) {
    XrTestTargetProfileFixture fixture;
    if (!xr_test_target_profile_fixture_init(&fixture, ilp32, runtime_profile))
        return NULL;

    XrTargetProviderContract providers[3] = {0};
    memcpy(providers, fixture.providers, sizeof(fixture.providers));
    uint8_t pointer_width = (uint8_t) fixture.input.machine.data_layout.pointer.size;
    uint8_t pointer_alignment = (uint8_t) fixture.input.machine.data_layout.pointer.align;
    XrTargetProviderCallSlotAbi status = make_call_slot(
        XR_TARGET_PROVIDER_CALL_VALUE_UNSIGNED_INTEGER, 1u, 1u,
        XR_TARGET_PROVIDER_CALL_OWNERSHIP_NONE, 0u);
    XrTargetProviderCallSlotAbi parameters[] = {
        make_call_slot(XR_TARGET_PROVIDER_CALL_VALUE_DATA_ADDRESS, pointer_width,
                       pointer_alignment, XR_TARGET_PROVIDER_CALL_OWNERSHIP_BORROWED, 0u),
        make_call_slot(XR_TARGET_PROVIDER_CALL_VALUE_DATA_ADDRESS, pointer_width,
                       pointer_alignment, XR_TARGET_PROVIDER_CALL_OWNERSHIP_BORROWED, 0u),
        make_call_slot(XR_TARGET_PROVIDER_CALL_VALUE_DATA_ADDRESS, pointer_width,
                       pointer_alignment, XR_TARGET_PROVIDER_CALL_OWNERSHIP_BORROWED, 0u),
    };
    XrTargetProviderCallAbiContract call_abi = make_call_abi(status, parameters, 3u);
    call_abi.target_endian = (uint8_t) fixture.input.machine.data_layout.endian;
    call_abi.pointer_width = pointer_width;
    call_abi.pointer_alignment = pointer_alignment;
    uint32_t availability =
        runtime_profile == XR_TARGET_RUNTIME_PROFILE_FREESTANDING
            ? XR_TARGET_PROVIDER_AVAILABLE_FREESTANDING
            : XR_TARGET_PROVIDER_AVAILABLE_HOSTED;
    providers[2] = (XrTargetProviderContract) {
        .schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .abi_schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .flags = availability,
        .operation_count = 1u,
        .runtime_profile = runtime_profile,
        .provider_kind = XR_TARGET_PROVIDER_IO,
    };
    providers[2].operations[0] = make_operation(
        0u, call_abi, XR_TARGET_PROVIDER_EFFECT_IO,
        XR_TARGET_PROVIDER_LIFETIME_BORROWS,
        XR_TARGET_PROVIDER_FAILURE_RETURNS_STATUS);
    XrFingerprint key_digest;
    if (!xr_stable_id_from_key(XR_PROVIDER_IO_CONTRACT_KEY,
                               &providers[2].contract_id, &key_digest) ||
        !xr_stable_id_from_key(XR_PROVIDER_IO_PIPE_OPEN_OPERATION_KEY,
                               &providers[2].operations[0].stable_id, &key_digest))
        return NULL;
    fixture.input.providers = providers;
    fixture.input.provider_count = 3u;
    XrTargetProfile *profile = NULL;
    char error[512] = {0};
    if (!xr_target_profile_build(&fixture.input, &profile, error, sizeof(error))) {
        fprintf(stderr, "pipe target profile fixture failed: %s\n", error);
        return NULL;
    }
    return profile;
}
