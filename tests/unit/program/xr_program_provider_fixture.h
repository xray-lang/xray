/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_provider_fixture.h - Explicit logical contracts for test hosts
 */

#ifndef XR_PROGRAM_PROVIDER_FIXTURE_H
#define XR_PROGRAM_PROVIDER_FIXTURE_H

#include "plan/semantic/xr_semantic_ids.h"
#include "runtime/abi/xr_provider_logical_contract.h"

static inline XrProviderLogicalContract xr_program_fixture_provider_contract(void) {
    XrProviderLogicalContract contract = {
        .schema_version = XR_PROVIDER_LOGICAL_SCHEMA_VERSION,
        .platforms = XR_PROVIDER_LOGICAL_PLATFORMS_ALL,
        .runtime_profiles = XR_PROVIDER_LOGICAL_PROFILE_HOSTED,
        .result_owner = XR_PROVIDER_OWNER_TRIVIAL,
        .error_owner = XR_PROVIDER_OWNER_TRIVIAL,
        .threads = XR_PROVIDER_THREADS_ANY,
        .reentry = XR_PROVIDER_REENTRY_ALLOWED,
        .callbacks = XR_PROVIDER_CALLBACK_NONE,
        .refusal = XR_PROVIDER_REFUSAL_TRAP,
    };
    return contract;
}

static inline XrProviderLogicalContract xr_program_fixture_scalar_contract(bool nullary) {
    XrProviderLogicalContract contract = xr_program_fixture_provider_contract();
    contract.effects = XR_PROVIDER_EFFECT_READS_CLOCK;
    contract.parameter_count = nullary ? 0u : 1u;
    contract.type_byte_count = nullary ? 2u : 3u;
    contract.types[0] = XR_PROVIDER_TYPE_I64;
    contract.types[1] = XR_PROVIDER_TYPE_UNIT;
    if (!nullary) {
        contract.parameter_modes[0] = XR_PROVIDER_MODE_IN;
        contract.parameter_owners[0] = XR_PROVIDER_OWNER_TRIVIAL;
        contract.types[1] = XR_PROVIDER_TYPE_I64;
        contract.types[2] = XR_PROVIDER_TYPE_UNIT;
    }
    return contract;
}

static inline XrProviderLogicalContract xr_program_fixture_pipe_contract(void) {
    XrProviderLogicalContract contract = xr_program_fixture_provider_contract();
    contract.platforms =
        XR_PROVIDER_PLATFORM_LINUX | XR_PROVIDER_PLATFORM_MACOS | XR_PROVIDER_PLATFORM_WINDOWS;
    contract.effects = XR_PROVIDER_EFFECT_IO;
    contract.type_byte_count = 6u;
    contract.types[0] = XR_PROVIDER_TYPE_OPTIONAL;
    contract.types[1] = XR_PROVIDER_TYPE_TUPLE;
    contract.types[2] = 2u;
    contract.types[3] = XR_PROVIDER_TYPE_I64;
    contract.types[4] = XR_PROVIDER_TYPE_I64;
    contract.types[5] = XR_PROVIDER_TYPE_UNIT;
    contract.resource_count = 2u;
    XrStableId resource = {{0}};
    XrFingerprint fingerprint;
    if (!xr_stable_id_from_key("xray.runtime.resource.v1/pipe-endpoint", &resource, &fingerprint))
        return (XrProviderLogicalContract) {0};
    for (uint8_t index = 0u; index < 2u; ++index) {
        contract.resources[index] = (XrProviderLogicalResourceTransition) {
            .resource_id = resource,
            .source = XR_PROVIDER_RESOURCE_RESULT,
            .action = XR_PROVIDER_RESOURCE_ACQUIRE,
            .timing = XR_PROVIDER_RESOURCE_RESULT_PRESENT,
            .path_count = 2u,
            .path = {0u, index},
        };
    }
    return contract;
}

#endif  // XR_PROGRAM_PROVIDER_FIXTURE_H
