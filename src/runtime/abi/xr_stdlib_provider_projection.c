/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_stdlib_provider_projection.c - Target projection of declared host adapters
 *
 * KEY CONCEPT:
 *   A declared adapter chooses the physical signature. Logical facts are
 *   independently decoded and checked before any target contract is published.
 */

#include "xr_stdlib_provider_projection.h"

#include <string.h>

static uint32_t logical_platform(uint16_t operating_system) {
    switch (operating_system) {
        case XR_TARGET_OS_LINUX:
            return XR_PROVIDER_PLATFORM_LINUX;
        case XR_TARGET_OS_MACOS:
            return XR_PROVIDER_PLATFORM_MACOS;
        case XR_TARGET_OS_WINDOWS:
            return XR_PROVIDER_PLATFORM_WINDOWS;
        default:
            return 0u;
    }
}

static bool logical_signature_matches(const XrProviderLogicalContract *logical, uint8_t adapter) {
    static const uint8_t nullary_i64[] = {XR_PROVIDER_TYPE_I64, XR_PROVIDER_TYPE_UNIT};
    static const uint8_t unary_i64[] = {XR_PROVIDER_TYPE_I64, XR_PROVIDER_TYPE_I64,
                                        XR_PROVIDER_TYPE_UNIT};
    static const uint8_t unary_bool[] = {XR_PROVIDER_TYPE_I64, XR_PROVIDER_TYPE_BOOL,
                                         XR_PROVIDER_TYPE_UNIT};
    static const uint8_t optional_pair[] = {
        XR_PROVIDER_TYPE_OPTIONAL, XR_PROVIDER_TYPE_TUPLE, 2u,
        XR_PROVIDER_TYPE_I64,      XR_PROVIDER_TYPE_I64,   XR_PROVIDER_TYPE_UNIT};
    const uint8_t *types = NULL;
    size_t size = 0u;
    uint8_t parameters = 0u;
    uint8_t resources = 0u;
    switch (adapter) {
        case XR_STDLIB_PROVIDER_I64_NULLARY_U64:
        case XR_STDLIB_PROVIDER_I64_NULLARY_I64:
            types = nullary_i64;
            size = sizeof(nullary_i64);
            break;
        case XR_STDLIB_PROVIDER_I64_UNARY_STATUS_OUT:
            types = unary_i64;
            size = sizeof(unary_i64);
            parameters = 1u;
            break;
        case XR_STDLIB_PROVIDER_BOOL_I64_PIPE_CLOSE:
            types = unary_bool;
            size = sizeof(unary_bool);
            parameters = 1u;
            resources = 1u;
            break;
        case XR_STDLIB_PROVIDER_OPTIONAL_I64_PAIR_PIPE_CREATE:
            types = optional_pair;
            size = sizeof(optional_pair);
            resources = 2u;
            break;
        default:
            return false;
    }
    return logical->parameter_count == parameters && logical->resource_count == resources &&
           logical->type_byte_count == size && memcmp(logical->types, types, size) == 0;
}

static bool logical_is_admitted(const XrProviderLogicalContract *logical, uint8_t adapter) {
    const uint32_t unsupported = XR_PROVIDER_EFFECT_MAY_ERROR | XR_PROVIDER_EFFECT_MAY_PANIC |
                                 XR_PROVIDER_EFFECT_MAY_SUSPEND;
    if (logical->runtime_profiles != XR_PROVIDER_LOGICAL_PROFILE_HOSTED ||
        (logical->effects & unsupported) != 0u ||
        logical->result_owner != XR_PROVIDER_OWNER_TRIVIAL ||
        logical->error_owner != XR_PROVIDER_OWNER_TRIVIAL ||
        logical->threads != XR_PROVIDER_THREADS_ANY ||
        logical->reentry != XR_PROVIDER_REENTRY_ALLOWED ||
        logical->callbacks != XR_PROVIDER_CALLBACK_NONE ||
        !logical_signature_matches(logical, adapter))
        return false;
    for (uint8_t index = 0u; index < logical->parameter_count; ++index)
        if (logical->parameter_modes[index] != XR_PROVIDER_MODE_IN ||
            logical->parameter_owners[index] != XR_PROVIDER_OWNER_TRIVIAL)
            return false;
    return true;
}

static XrTargetProviderCallSlotAbi slot(uint8_t kind, uint8_t width, uint8_t alignment,
                                        uint8_t ownership) {
    return (XrTargetProviderCallSlotAbi) {
        .value_kind = kind,
        .width = width,
        .alignment = alignment,
        .ownership = ownership,
    };
}

static bool project_adapter(uint8_t adapter, XrTargetProviderOperationContract *operation,
                            const XrTargetDataLayout *layout) {
    if (layout->i64.size != 8u || layout->i64.align == 0u || layout->i64.align > 8u ||
        layout->pointer.size == 0u || layout->pointer.size > 8u || layout->pointer.align == 0u ||
        layout->pointer.align > 8u ||
        (layout->endian != XR_TARGET_ENDIAN_LITTLE && layout->endian != XR_TARGET_ENDIAN_BIG))
        return false;
    XrTargetProviderCallAbiContract *abi = &operation->call_abi;
    *abi = (XrTargetProviderCallAbiContract) {
        .schema_version = XR_RUNTIME_ABI_SCHEMA_VERSION,
        .calling_convention = XR_TARGET_PROVIDER_CALLING_CONVENTION_C,
        .target_endian = layout->endian == XR_TARGET_ENDIAN_LITTLE ? XR_RUNTIME_ENDIAN_LITTLE
                                                                   : XR_RUNTIME_ENDIAN_BIG,
        .pointer_width = (uint8_t) layout->pointer.size,
        .pointer_alignment = (uint8_t) layout->pointer.align,
        .result = slot(XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER, 8u,
                       (uint8_t) layout->i64.align, XR_TARGET_PROVIDER_CALL_OWNERSHIP_NONE),
    };
    switch (adapter) {
        case XR_STDLIB_PROVIDER_I64_NULLARY_U64:
        case XR_STDLIB_PROVIDER_I64_NULLARY_I64:
            break;
        case XR_STDLIB_PROVIDER_I64_UNARY_STATUS_OUT:
            abi->parameter_count = 1u;
            abi->parameters[0] = abi->result;
            break;
        case XR_STDLIB_PROVIDER_BOOL_I64_PIPE_CLOSE:
            abi->parameter_count = 1u;
            abi->parameters[0] = abi->result;
            abi->parameters[0].ownership = XR_TARGET_PROVIDER_CALL_OWNERSHIP_CONSUMED;
            abi->result = slot(XR_TARGET_PROVIDER_CALL_VALUE_UNSIGNED_INTEGER, 1u, 1u,
                               XR_TARGET_PROVIDER_CALL_OWNERSHIP_NONE);
            operation->lifetime_flags = XR_TARGET_PROVIDER_LIFETIME_CONSUMES_OWNED;
            break;
        case XR_STDLIB_PROVIDER_OPTIONAL_I64_PAIR_PIPE_CREATE:
            abi->parameter_count = 3u;
            abi->result = slot(XR_TARGET_PROVIDER_CALL_VALUE_UNSIGNED_INTEGER, 1u, 1u,
                               XR_TARGET_PROVIDER_CALL_OWNERSHIP_NONE);
            for (uint16_t index = 0u; index < abi->parameter_count; ++index)
                abi->parameters[index] =
                    slot(XR_TARGET_PROVIDER_CALL_VALUE_DATA_ADDRESS, abi->pointer_width,
                         abi->pointer_alignment, XR_TARGET_PROVIDER_CALL_OWNERSHIP_BORROWED);
            operation->lifetime_flags = XR_TARGET_PROVIDER_LIFETIME_BORROWS;
            operation->failure_flags = XR_TARGET_PROVIDER_FAILURE_RETURNS_STATUS;
            break;
        default:
            return false;
    }
    XrFingerprint fingerprint;
    return xr_target_provider_call_abi_fingerprint(abi, &fingerprint) == XR_RUNTIME_ABI_OK;
}

XR_FUNCDEF XrStdlibProviderProjectionStatus xr_stdlib_provider_project(
    const XrStdlibProviderDescriptor *descriptor, const XrTargetMachineFacts *machine,
    XrTargetProviderOperationContract *out) {
    XrProviderLogicalContract logical;
    if (!descriptor || !machine || !out ||
        xr_stdlib_provider_find(descriptor->contract_id, descriptor->operation_id) != descriptor ||
        !xr_stdlib_provider_logical(descriptor, &logical) ||
        !logical_is_admitted(&logical, descriptor->adapter))
        return XR_STDLIB_PROVIDER_PROJECTION_INVALID;
    if (machine->runtime_profile != XR_TARGET_RUNTIME_PROFILE_HOSTED ||
        (logical.platforms & logical_platform(machine->operating_system)) == 0u)
        return XR_STDLIB_PROVIDER_PROJECTION_UNAVAILABLE;
    XrTargetProviderOperationContract operation = {
        .stable_id = descriptor->operation_id,
        .logical_contract = logical,
    };
    if (logical.effects & XR_PROVIDER_EFFECT_IO)
        operation.effect_flags |= XR_TARGET_PROVIDER_EFFECT_IO;
    if (logical.effects & XR_PROVIDER_EFFECT_MANAGED_ALLOCATION)
        operation.effect_flags |= XR_TARGET_PROVIDER_EFFECT_ALLOCATES;
    if (logical.effects & XR_PROVIDER_EFFECT_MANAGED_DEALLOCATION)
        operation.effect_flags |= XR_TARGET_PROVIDER_EFFECT_DEALLOCATES;
    if (!project_adapter(descriptor->adapter, &operation, &machine->data_layout))
        return XR_STDLIB_PROVIDER_PROJECTION_INVALID;
    *out = operation;
    return XR_STDLIB_PROVIDER_PROJECTION_OK;
}
