/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_runtime_target_profile.c - Runtime-owned native TargetProfile projection
 */

#include "xr_runtime_target_profile.h"
#include "xr_runtime_target_authority.h"
#include "../../plan/target/xr_target_profile.h"

#include <stdio.h>

static void set_error(char *error, size_t size, const char *detail) {
    if (error && size)
        snprintf(error, size, "XR_TARGET_1000: %s", detail);
}

static void set_runtime_error(char *error, size_t size, const char *detail,
                              XrRuntimeAbiStatus status) {
    if (error && size)
        snprintf(error, size, "XR_TARGET_1000: %s: %s", detail,
                 xr_runtime_abi_status_name(status));
}

bool xr_runtime_target_profile_build_native_hosted(XrTargetProfile **out,
                                                   char *error,
                                                   size_t error_size) {
    if (out)
        *out = NULL;
    if (!out) {
        set_error(error, error_size,
                  "native hosted target profile output is missing");
        return false;
    }

    XrRuntimeTargetAuthority authority;
    XrRuntimeAbiStatus status =
        xr_runtime_target_authority_native_hosted(&authority);
    if (status != XR_RUNTIME_ABI_OK) {
        set_runtime_error(error, error_size,
                          "native hosted runtime authority is invalid", status);
        return false;
    }
    XrTargetProfileBuildInput input = {
        .machine = authority.machine,
        .runtime_abi = &authority.runtime_abi,
        .object_header_materialization =
            &authority.object_header_materialization,
        .string_contract = &authority.string_contract,
        .providers = authority.providers,
        .provider_count = authority.provider_count,
    };
    return xr_target_profile_build(&input, out, error, error_size);
}
