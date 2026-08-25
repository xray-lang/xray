/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xisolate_profile.c - Profile-based isolate creation
 *
 * KEY CONCEPT:
 *   Each profile starts from the all-zero VM configuration; callers override
 *   only what they need (trace, workers, etc.).  Creation uses the explicit full VM
 *   constructor. This eliminates the repeated
 *   XrVMConfig boilerplate across run/repl/test/check/fmt/compile/
 *   deps/eval and the MCP analyzer isolate.
 */

#include "xisolate_profile.h"
#include "../base/xchecks.h"
#include "../plan/target/xr_target_profile.h"
#include "../toolchain/xcompiler_session.h"
#include <stdio.h>

/* ========== Profile Configuration ========== */

void xr_isolate_profile_params(XrIsolateProfile profile, XrVMConfig *out) {
    XR_DCHECK(out != NULL, "out must not be NULL");

    /* The all-zero representation is the default configuration. */
    *out = (XrVMConfig){0};

    (void) profile;
}

/* ========== Create ========== */

static bool install_native_hosted_profile(XrVMRuntime *isolate) {
    XrCompilerSession *session =
        xr_compiler_session_current_for_isolate(isolate);
    XrTargetProfile *profile = NULL;
    char error[256] = {0};
    if (!session || !xr_target_profile_build_native_hosted(
                        &profile, error, sizeof(error))) {
        fprintf(stderr, "xray: %s\n",
                error[0] ? error : "native hosted TargetProfile is unavailable");
        return false;
    }
    bool installed = xr_compiler_session_set_target_profile(session, profile);
    const XrTargetProfile *session_profile =
        xr_compiler_session_target_profile(session);
    installed = installed && session_profile &&
                xr_target_profile_require_exact(profile, session_profile,
                                                error, sizeof(error));
    xr_target_profile_free(profile);
    if (!installed)
        fprintf(stderr, "xray: exact native hosted TargetProfile installation failed\n");
    return installed;
}

XrVMRuntime *xr_isolate_profile_create(const XrVMConfig *params) {
    XR_DCHECK(params != NULL, "params must not be NULL");

    XrVMRuntime *iso = xray_vm_new_full(params);
    if (!iso) {
        fprintf(stderr, "xray: failed to create isolate\n");
    } else if (!install_native_hosted_profile(iso)) {
        xray_vm_delete(iso);
        iso = NULL;
    }
    return iso;
}

XrVMRuntime *xr_isolate_profile_new(XrIsolateProfile profile) {
    XrVMConfig params;
    xr_isolate_profile_params(profile, &params);
    return xr_isolate_profile_create(&params);
}
