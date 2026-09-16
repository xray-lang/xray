/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_execution_identity.c - Pure program/profile execution identity
 */

#include "xr_execution_identity.h"

#include "../base/xsha256.h"

#include <string.h>

static void hash_identity(XrSHA256Context *context, XrFingerprint identity) {
    xr_sha256_update(context, identity.bytes, sizeof(identity.bytes));
}

static bool provider_requirements_match(const XrValidatedProgram *program,
                                        const XrTargetProfile *profile) {
    uint32_t count = xr_validated_program_provider_requirement_count(program);
    for (uint32_t index = 0u; index < count; ++index) {
        XrProgramProviderRequirementView requirement = {0};
        if (!xr_validated_program_provider_requirement(program, index, &requirement))
            return false;
        const XrTargetProviderContract *provider = NULL;
        for (size_t candidate = 0u; candidate < xr_target_profile_provider_count(profile);
             ++candidate) {
            const XrTargetProviderContract *value = xr_target_profile_provider(profile, candidate);
            if (value && memcmp(value->contract_id.bytes, requirement.contract_id.bytes,
                                XR_STABLE_ID_BYTES) == 0) {
                provider = value;
                break;
            }
        }
        if (!provider || provider->provider_role != XR_TARGET_PROVIDER_ROLE_OPERATIONS)
            return false;
        for (uint32_t operation = 0u; operation < requirement.operation_count; ++operation) {
            const XrProgramProviderOperationRequirement *required =
                &requirement.operations[operation];
            bool matched = false;
            for (uint16_t candidate = 0u; candidate < provider->operation_count; ++candidate) {
                const XrTargetProviderOperationContract *value = &provider->operations[candidate];
                if (memcmp(value->stable_id.bytes, required->operation_id.bytes,
                           XR_STABLE_ID_BYTES) == 0) {
                    matched = xr_provider_logical_contract_equal(&required->logical_contract,
                                                                 &value->logical_contract);
                    break;
                }
            }
            if (!matched)
                return false;
        }
    }
    return true;
}

bool xr_execution_id_compute(const XrValidatedProgram *program,
                             const XrTargetProfile *profile,
                             XrExecutionId *execution_id_out) {
    if (execution_id_out)
        memset(execution_id_out, 0, sizeof(*execution_id_out));
    if (!program || !profile || !execution_id_out || !xr_target_profile_verify(profile, NULL, 0) ||
        !provider_requirements_match(program, profile))
        return false;

    const XrBoundaryAbi *boundary = xr_target_profile_boundary_abi(profile);
    const XrRuntimeKernelContract *kernel = xr_target_profile_runtime_kernel(profile);
    if (!boundary || !kernel)
        return false;

    static const uint8_t domain[] = "xray-execution-id-v1\0";
    XrSHA256Context context;
    XrProgramId program_id = xr_validated_program_id(program);
    XrFingerprint profile_id = xr_target_profile_fingerprint(profile);
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    xr_sha256_update(&context, program_id.bytes, sizeof(program_id.bytes));
    hash_identity(&context, profile_id);
    hash_identity(&context, boundary->id);
    hash_identity(&context, kernel->id);
    xr_sha256_final(&context, execution_id_out->bytes);
    return true;
}
