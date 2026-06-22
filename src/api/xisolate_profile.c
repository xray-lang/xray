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
 *   Each profile starts from bytecode VM defaults; callers override only what
 *   they need (trace, workers, etc.).  Creation uses the explicit full VM
 *   constructor. This eliminates the repeated
 *   XrVMConfig boilerplate across run/repl/test/check/fmt/compile/
 *   deps/eval and the MCP analyzer isolate.
 */

#include "xisolate_profile.h"
#include "../base/xchecks.h"
#include <stdio.h>
#include <string.h>

/* ========== Profile Configuration ========== */

void xr_isolate_profile_params(XrIsolateProfile profile, XrVMConfig *out) {
    XR_DCHECK(out != NULL, "out must not be NULL");

    /* Start with defaults */
    xray_vm_config_init(out);

    (void) profile;
}

/* ========== Create ========== */

XrVMRuntime *xr_isolate_profile_create(const XrVMConfig *params) {
    XR_DCHECK(params != NULL, "params must not be NULL");

    XrVMRuntime *iso = xray_vm_new_full(params);
    if (!iso) {
        fprintf(stderr, "xray: failed to create isolate\n");
    }
    return iso;
}

XrVMRuntime *xr_isolate_profile_new(XrIsolateProfile profile) {
    XrVMConfig params;
    xr_isolate_profile_params(profile, &params);
    return xr_isolate_profile_create(&params);
}
