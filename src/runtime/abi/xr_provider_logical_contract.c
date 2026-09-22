/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_provider_logical_contract.c - Canonical logical provider contract encoding
 *
 * KEY CONCEPT:
 *   Verification precedes publication. Fingerprints cover logical values,
 *   effects, resource transitions and execution constraints without including
 *   a host ABI, implementation name or compiler data structure.
 */

#include "xr_provider_logical_contract.h"
#include "../../base/xsha256.h"

#include <string.h>

_Static_assert(22u + 2u * XR_PROVIDER_LOGICAL_MAX_PARAMETERS + XR_PROVIDER_LOGICAL_MAX_TYPE_BYTES +
                       XR_PROVIDER_LOGICAL_MAX_RESOURCES *
                           (XR_STABLE_ID_BYTES + 5u + XR_PROVIDER_LOGICAL_MAX_PATH) <=
                   XR_PROVIDER_LOGICAL_MAX_ENCODED_BYTES,
               "the complete logical contract fits its canonical encoding buffer");

static bool all_zero(const void *data, size_t size) {
    const uint8_t *bytes = data;
    for (size_t index = 0; index < size; ++index)
        if (bytes[index] != 0u)
            return false;
    return true;
}

static bool resource_is_zero(const XrProviderLogicalResourceTransition *resource) {
    return all_zero(resource->resource_id.bytes, sizeof(resource->resource_id.bytes)) &&
           resource->source == 0u && resource->ordinal == 0u && resource->action == 0u &&
           resource->timing == 0u && resource->path_count == 0u &&
           all_zero(resource->path, sizeof(resource->path)) &&
           all_zero(resource->reserved, sizeof(resource->reserved));
}

XR_FUNCDEF bool xr_provider_logical_contract_is_zero(const XrProviderLogicalContract *contract) {
    if (!contract || contract->schema_version != 0u || contract->effects != 0u ||
        contract->platforms != 0u || contract->runtime_profiles != 0u ||
        contract->parameter_count != 0u || contract->type_byte_count != 0u ||
        contract->resource_count != 0u || contract->result_owner != 0u ||
        contract->error_owner != 0u || contract->threads != 0u || contract->reentry != 0u ||
        contract->callbacks != 0u || contract->refusal != 0u ||
        !all_zero(contract->reserved, sizeof(contract->reserved)) ||
        !all_zero(contract->parameter_modes, sizeof(contract->parameter_modes)) ||
        !all_zero(contract->parameter_owners, sizeof(contract->parameter_owners)) ||
        !all_zero(contract->types, sizeof(contract->types)))
        return false;
    for (size_t index = 0u; index < XR_PROVIDER_LOGICAL_MAX_RESOURCES; ++index)
        if (!resource_is_zero(&contract->resources[index]))
            return false;
    return true;
}

XR_FUNCDEF bool xr_provider_logical_contract_equal(const XrProviderLogicalContract *left,
                                                   const XrProviderLogicalContract *right) {
    uint8_t left_bytes[XR_PROVIDER_LOGICAL_MAX_ENCODED_BYTES];
    uint8_t right_bytes[XR_PROVIDER_LOGICAL_MAX_ENCODED_BYTES];
    size_t left_size = 0u;
    size_t right_size = 0u;
    return xr_provider_logical_contract_encode(left, left_bytes, sizeof(left_bytes), &left_size) &&
           xr_provider_logical_contract_encode(right, right_bytes, sizeof(right_bytes),
                                               &right_size) &&
           left_size == right_size && memcmp(left_bytes, right_bytes, left_size) == 0;
}

static bool type_size(const uint8_t *bytes, uint8_t available, uint8_t *size_out,
                      bool *resource_out) {
    bool resource = false;
    uint16_t pending = 1u;
    uint8_t offset = 0u;
    while (pending != 0u) {
        if (offset >= available)
            return false;
        uint8_t token = bytes[offset++];
        --pending;
        switch (token) {
            case XR_PROVIDER_TYPE_UNIT:
            case XR_PROVIDER_TYPE_BOOL:
            case XR_PROVIDER_TYPE_I64:
            case XR_PROVIDER_TYPE_BYTES:
                break;
            case XR_PROVIDER_TYPE_RESOURCE:
                if ((uint32_t) available - offset < XR_STABLE_ID_BYTES ||
                    all_zero(bytes + offset, XR_STABLE_ID_BYTES))
                    return false;
                offset += XR_STABLE_ID_BYTES;
                resource = true;
                break;
            case XR_PROVIDER_TYPE_OPTIONAL:
                ++pending;
                break;
            case XR_PROVIDER_TYPE_TUPLE:
                if (offset >= available || bytes[offset] == 0u)
                    return false;
                pending += bytes[offset++];
                break;
            default:
                return false;
        }
        if (pending > available - offset)
            return false;
    }
    if (resource_out) *resource_out = resource;
    *size_out = offset;
    return true;
}

XR_FUNCDEF bool xr_provider_logical_contract_type(const XrProviderLogicalContract *contract,
                                                  uint8_t index, XrProviderLogicalTypeView *out) {
    if (!contract || !out || contract->parameter_count > XR_PROVIDER_LOGICAL_MAX_PARAMETERS ||
        contract->type_byte_count > XR_PROVIDER_LOGICAL_MAX_TYPE_BYTES ||
        index > contract->parameter_count + 1u)
        return false;
    uint8_t offset = 0u;
    for (uint8_t current = 0u; current <= index; ++current) {
        uint8_t size = 0u;
        if (!type_size(contract->types + offset, contract->type_byte_count - offset, &size, NULL))
            return false;
        if (current == index) {
            *out = (XrProviderLogicalTypeView) {contract->types + offset, size};
            return true;
        }
        offset += size;
    }
    return false;
}

static bool type_child(XrProviderLogicalTypeView parent, uint8_t index,
                       XrProviderLogicalTypeView *out) {
    uint8_t offset = 0u;
    if (parent.bytes[0] == XR_PROVIDER_TYPE_OPTIONAL) {
        if (index != 0u)
            return false;
        offset = 1u;
    } else if (parent.bytes[0] == XR_PROVIDER_TYPE_TUPLE) {
        if (parent.size < 2u || index >= parent.bytes[1])
            return false;
        offset = 2u;
    } else {
        return false;
    }
    for (uint8_t current = 0u; current <= index; ++current) {
        uint8_t size = 0u;
        if (!type_size(parent.bytes + offset, parent.size - offset, &size, NULL))
            return false;
        if (current == index) {
            *out = (XrProviderLogicalTypeView) {parent.bytes + offset, size};
            return true;
        }
        offset += size;
    }
    return false;
}

static int compare_resource_values(const XrProviderLogicalResourceTransition *left,
                                   const XrProviderLogicalResourceTransition *right) {
    if (left->source != right->source)
        return left->source < right->source ? -1 : 1;
    if (left->ordinal != right->ordinal)
        return left->ordinal < right->ordinal ? -1 : 1;
    uint8_t count = left->path_count < right->path_count ? left->path_count : right->path_count;
    int order = memcmp(left->path, right->path, count);
    if (order != 0)
        return order;
    return left->path_count < right->path_count ? -1 : left->path_count > right->path_count;
}

XR_FUNCDEF bool xr_provider_logical_resource_type_id(XrProviderLogicalTypeView type,
                                                     XrStableId *out) {
    if (!out || !type.bytes || type.size != 1u + XR_STABLE_ID_BYTES ||
        type.bytes[0] != XR_PROVIDER_TYPE_RESOURCE || all_zero(type.bytes + 1u, XR_STABLE_ID_BYTES))
        return false;
    memcpy(out->bytes, type.bytes + 1u, XR_STABLE_ID_BYTES);
    return true;
}

static bool resource_valid(const XrProviderLogicalContract *contract,
                           const XrProviderLogicalResourceTransition *resource) {
    if (all_zero(&resource->resource_id, sizeof(resource->resource_id)) ||
        resource->path_count > XR_PROVIDER_LOGICAL_MAX_PATH ||
        !all_zero(resource->path + resource->path_count,
                  XR_PROVIDER_LOGICAL_MAX_PATH - resource->path_count) ||
        !all_zero(resource->reserved, sizeof(resource->reserved)))
        return false;
    uint8_t index = 0u;
    if (resource->source == XR_PROVIDER_RESOURCE_PARAMETER) {
        if (resource->ordinal >= contract->parameter_count ||
            resource->action != XR_PROVIDER_RESOURCE_CONSUME ||
            resource->timing != XR_PROVIDER_RESOURCE_CALL_ENTER)
            return false;
        index = resource->ordinal;
    } else if (resource->source == XR_PROVIDER_RESOURCE_RESULT) {
        if (resource->ordinal != 0u || resource->action != XR_PROVIDER_RESOURCE_ACQUIRE ||
            resource->timing != XR_PROVIDER_RESOURCE_RESULT_PRESENT || resource->path_count == 0u ||
            resource->path[0] != 0u)
            return false;
        index = contract->parameter_count;
    } else {
        return false;
    }
    XrProviderLogicalTypeView type = {0};
    if (!xr_provider_logical_contract_type(contract, index, &type) ||
        (resource->source == XR_PROVIDER_RESOURCE_RESULT &&
         type.bytes[0] != XR_PROVIDER_TYPE_OPTIONAL))
        return false;
    for (uint8_t depth = 0u; depth < resource->path_count; ++depth)
        if (!type_child(type, resource->path[depth], &type))
            return false;
    if (type.size == 1u && type.bytes[0] == XR_PROVIDER_TYPE_I64)
        return true;
    XrStableId identity;
    return xr_provider_logical_resource_type_id(type, &identity) &&
           memcmp(identity.bytes, resource->resource_id.bytes, XR_STABLE_ID_BYTES) == 0;
}

static bool ownership_valid(uint8_t owner) {
    return owner >= XR_PROVIDER_OWNER_TRIVIAL && owner <= XR_PROVIDER_OWNER_OWNED;
}

XR_FUNCDEF bool xr_provider_logical_contract_verify(const XrProviderLogicalContract *contract) {
    if (!contract || contract->schema_version != XR_PROVIDER_LOGICAL_SCHEMA_VERSION ||
        contract->parameter_count > XR_PROVIDER_LOGICAL_MAX_PARAMETERS ||
        contract->type_byte_count > XR_PROVIDER_LOGICAL_MAX_TYPE_BYTES ||
        contract->resource_count > XR_PROVIDER_LOGICAL_MAX_RESOURCES ||
        (contract->effects & ~XR_PROVIDER_LOGICAL_EFFECTS_ALL) != 0u || contract->platforms == 0u ||
        (contract->platforms & ~XR_PROVIDER_LOGICAL_PLATFORMS_ALL) != 0u ||
        contract->runtime_profiles == 0u || (contract->runtime_profiles & ~UINT8_C(3)) != 0u ||
        !ownership_valid(contract->result_owner) || !ownership_valid(contract->error_owner) ||
        contract->threads < XR_PROVIDER_THREADS_ANY ||
        contract->threads > XR_PROVIDER_THREADS_INSTANCE_AFFINE ||
        contract->reentry < XR_PROVIDER_REENTRY_ALLOWED ||
        contract->reentry > XR_PROVIDER_REENTRY_FORBIDDEN ||
        contract->callbacks < XR_PROVIDER_CALLBACK_NONE ||
        contract->callbacks > XR_PROVIDER_CALLBACK_SYNCHRONOUS ||
        contract->refusal != XR_PROVIDER_REFUSAL_TRAP ||
        !all_zero(contract->reserved, sizeof(contract->reserved)))
        return false;
    for (uint8_t index = 0u; index < contract->parameter_count; ++index)
        if (contract->parameter_modes[index] < XR_PROVIDER_MODE_IN ||
            contract->parameter_modes[index] > XR_PROVIDER_MODE_OUT ||
            !ownership_valid(contract->parameter_owners[index]))
            return false;
    if (!all_zero(contract->parameter_modes + contract->parameter_count,
                  XR_PROVIDER_LOGICAL_MAX_PARAMETERS - contract->parameter_count) ||
        !all_zero(contract->parameter_owners + contract->parameter_count,
                  XR_PROVIDER_LOGICAL_MAX_PARAMETERS - contract->parameter_count) ||
        !all_zero(contract->types + contract->type_byte_count,
                  XR_PROVIDER_LOGICAL_MAX_TYPE_BYTES - contract->type_byte_count))
        return false;
    for (uint8_t index = 0u; index < contract->parameter_count + 2u; ++index) {
        XrProviderLogicalTypeView type = {0};
        uint8_t size = 0u;
        bool resource = false;
        if (!xr_provider_logical_contract_type(contract, index, &type) ||
            !type_size(type.bytes, type.size, &size, &resource) || size != type.size)
            return false;
        uint8_t owner = index < contract->parameter_count ? contract->parameter_owners[index]
                           : index == contract->parameter_count ? contract->result_owner
                                                               : contract->error_owner;
        if (resource && owner == XR_PROVIDER_OWNER_TRIVIAL)
            return false;
    }
    XrProviderLogicalTypeView error = {0};
    if (!xr_provider_logical_contract_type(contract, contract->parameter_count + 1u, &error) ||
        error.bytes + error.size != contract->types + contract->type_byte_count)
        return false;
    bool has_error = error.size != 1u || error.bytes[0] != XR_PROVIDER_TYPE_UNIT;
    if (has_error != ((contract->effects & XR_PROVIDER_EFFECT_MAY_ERROR) != 0u) ||
        (!has_error && contract->error_owner != XR_PROVIDER_OWNER_TRIVIAL))
        return false;
    for (uint8_t index = 0u; index < contract->resource_count; ++index)
        if (!resource_valid(contract, &contract->resources[index]) ||
            (index != 0u && compare_resource_values(&contract->resources[index - 1u],
                                                    &contract->resources[index]) >= 0))
            return false;
    for (uint8_t index = contract->resource_count; index < XR_PROVIDER_LOGICAL_MAX_RESOURCES;
         ++index)
        if (!resource_is_zero(&contract->resources[index]))
            return false;
    return true;
}

static void write_u32(uint8_t *bytes, uint32_t value) {
    for (uint8_t index = 0u; index < 4u; ++index)
        bytes[index] = (uint8_t) (value >> (8u * index));
}

static uint32_t read_u32(const uint8_t *bytes) {
    uint32_t value = 0u;
    for (uint8_t index = 0u; index < 4u; ++index)
        value |= (uint32_t) bytes[index] << (8u * index);
    return value;
}

XR_FUNCDEF bool xr_provider_logical_contract_encode(const XrProviderLogicalContract *contract,
                                                    uint8_t *out, size_t capacity,
                                                    size_t *size_out) {
    if (!out || !size_out || !xr_provider_logical_contract_verify(contract))
        return false;
    uint8_t bytes[XR_PROVIDER_LOGICAL_MAX_ENCODED_BYTES] = {0};
    write_u32(bytes, contract->schema_version);
    write_u32(bytes + 4u, contract->effects);
    write_u32(bytes + 8u, contract->platforms);
    const uint8_t header[] = {
        contract->runtime_profiles, contract->parameter_count, contract->type_byte_count,
        contract->resource_count,   contract->result_owner,    contract->error_owner,
        contract->threads,          contract->reentry,         contract->callbacks,
        contract->refusal,
    };
    size_t size = 12u;
    memcpy(bytes + size, header, sizeof(header));
    size += sizeof(header);
    for (uint8_t index = 0u; index < contract->parameter_count; ++index) {
        bytes[size++] = contract->parameter_modes[index];
        bytes[size++] = contract->parameter_owners[index];
    }
    memcpy(bytes + size, contract->types, contract->type_byte_count);
    size += contract->type_byte_count;
    for (uint8_t index = 0u; index < contract->resource_count; ++index) {
        const XrProviderLogicalResourceTransition *resource = &contract->resources[index];
        memcpy(bytes + size, resource->resource_id.bytes, XR_STABLE_ID_BYTES);
        size += XR_STABLE_ID_BYTES;
        bytes[size++] = resource->source;
        bytes[size++] = resource->ordinal;
        bytes[size++] = resource->action;
        bytes[size++] = resource->timing;
        bytes[size++] = resource->path_count;
        memcpy(bytes + size, resource->path, resource->path_count);
        size += resource->path_count;
    }
    if (size > capacity)
        return false;
    memcpy(out, bytes, size);
    *size_out = size;
    return true;
}

XR_FUNCDEF bool xr_provider_logical_contract_decode(const uint8_t *bytes, size_t size,
                                                    XrProviderLogicalContract *out) {
    if (!bytes || !out || size < 22u || size > XR_PROVIDER_LOGICAL_MAX_ENCODED_BYTES)
        return false;
    XrProviderLogicalContract contract = {
        .schema_version = read_u32(bytes),
        .effects = read_u32(bytes + 4u),
        .platforms = read_u32(bytes + 8u),
        .runtime_profiles = bytes[12],
        .parameter_count = bytes[13],
        .type_byte_count = bytes[14],
        .resource_count = bytes[15],
        .result_owner = bytes[16],
        .error_owner = bytes[17],
        .threads = bytes[18],
        .reentry = bytes[19],
        .callbacks = bytes[20],
        .refusal = bytes[21],
    };
    if (contract.parameter_count > XR_PROVIDER_LOGICAL_MAX_PARAMETERS ||
        contract.type_byte_count > XR_PROVIDER_LOGICAL_MAX_TYPE_BYTES ||
        contract.resource_count > XR_PROVIDER_LOGICAL_MAX_RESOURCES)
        return false;
    size_t offset = 22u;
    if (size - offset < 2u * contract.parameter_count + contract.type_byte_count)
        return false;
    for (uint8_t index = 0u; index < contract.parameter_count; ++index) {
        contract.parameter_modes[index] = bytes[offset++];
        contract.parameter_owners[index] = bytes[offset++];
    }
    memcpy(contract.types, bytes + offset, contract.type_byte_count);
    offset += contract.type_byte_count;
    for (uint8_t index = 0u; index < contract.resource_count; ++index) {
        XrProviderLogicalResourceTransition *resource = &contract.resources[index];
        if (size - offset < XR_STABLE_ID_BYTES + 5u)
            return false;
        memcpy(resource->resource_id.bytes, bytes + offset, XR_STABLE_ID_BYTES);
        offset += XR_STABLE_ID_BYTES;
        resource->source = bytes[offset++];
        resource->ordinal = bytes[offset++];
        resource->action = bytes[offset++];
        resource->timing = bytes[offset++];
        resource->path_count = bytes[offset++];
        if (resource->path_count > XR_PROVIDER_LOGICAL_MAX_PATH ||
            size - offset < resource->path_count)
            return false;
        memcpy(resource->path, bytes + offset, resource->path_count);
        offset += resource->path_count;
    }
    if (offset != size || !xr_provider_logical_contract_verify(&contract))
        return false;
    *out = contract;
    return true;
}

XR_FUNCDEF bool xr_provider_logical_contract_fingerprint(const XrProviderLogicalContract *contract,
                                                         XrFingerprint *out) {
    uint8_t bytes[XR_PROVIDER_LOGICAL_MAX_ENCODED_BYTES];
    size_t size = 0u;
    if (!out || !xr_provider_logical_contract_encode(contract, bytes, sizeof(bytes), &size))
        return false;
    static const uint8_t domain[] = "xray-provider-logical-contract-v1\0";
    XrSHA256Context context;
    XrFingerprint fingerprint;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    xr_sha256_update(&context, bytes, size);
    xr_sha256_final(&context, fingerprint.bytes);
    *out = fingerprint;
    return true;
}
