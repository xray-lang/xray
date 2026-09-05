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

bool xr_execution_id_compute(const XrValidatedProgram *program,
                             const XrTargetProfile *profile,
                             XrExecutionId *execution_id_out) {
    if (execution_id_out)
        memset(execution_id_out, 0, sizeof(*execution_id_out));
    if (!program || !profile || !execution_id_out ||
        !xr_target_profile_verify(profile, NULL, 0))
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
