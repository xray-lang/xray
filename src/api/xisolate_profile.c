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
 *   Each profile selects appropriate init_flags; callers override only what
 *   they need (JIT, trace, workers, etc.).  This eliminates the repeated
 *   XrayIsolateParams boilerplate across run/repl/test/check/fmt/compile/
 *   deps/eval and the MCP analyzer isolate.
 */

#include "xisolate_profile.h"
#include "../base/xchecks.h"
#include <stdio.h>
#include <string.h>

/* ========== Profile Configuration ========== */

void xr_isolate_profile_params(XrIsolateProfile profile, XrayIsolateParams *out) {
    XR_DCHECK(out != NULL, "out must not be NULL");

    /* Start with defaults */
    xray_isolate_params_init(out);

    switch (profile) {
        case XR_ISOLATE_PROFILE_RUN:
        case XR_ISOLATE_PROFILE_EVAL:
        case XR_ISOLATE_PROFILE_TEST:
            /* Full runtime: all subsystems */
            xray_isolate_setup_full(out);
            break;

        case XR_ISOLATE_PROFILE_REPL:
            /* Full runtime, but JIT is force-disabled.  Each REPL input
             * is a one-shot top-level proto; tier-up call-count
             * thresholds will never be reached for the new code, and
             * compiling the same proto on every input would only add
             * latency to interactive prompts.  Cross-input shared
             * variables also bypass the shapes JIT relies on.  Keep
             * REPL on the interpreter path exclusively — predictable
             * latency wins over peak throughput here. */
            xray_isolate_setup_full(out);
            out->enable_jit = false;
            break;

        case XR_ISOLATE_PROFILE_PARSE:
            /* Minimal: just compiler + source cache */
            out->init_flags = XR_INIT_VM | XR_INIT_GC | XR_INIT_COMPILER | XR_INIT_SOURCE_CACHE;
            xray_isolate_setup_full(out);
            break;

        case XR_ISOLATE_PROFILE_ANALYZE:
            /* Compiler + analyzer + source cache */
            out->init_flags = XR_INIT_VM | XR_INIT_GC | XR_INIT_COMPILER | XR_INIT_ANALYZER |
                              XR_INIT_SOURCE_CACHE | XR_INIT_CLASSES | XR_INIT_SYMBOLS;
            xray_isolate_setup_full(out);
            break;
    }
}

/* ========== Create ========== */

XrayIsolate *xr_isolate_profile_create(const XrayIsolateParams *params) {
    XR_DCHECK(params != NULL, "params must not be NULL");

    XrayIsolate *iso = xray_isolate_new(params);
    if (!iso) {
        fprintf(stderr, "xray: failed to create isolate\n");
    }
    return iso;
}

XrayIsolate *xr_isolate_profile_new(XrIsolateProfile profile) {
    XrayIsolateParams params;
    xr_isolate_profile_params(profile, &params);
    return xr_isolate_profile_create(&params);
}
