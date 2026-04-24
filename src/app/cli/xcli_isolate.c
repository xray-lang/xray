/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_isolate.c - Profile-based isolate creation for CLI commands
 *
 * KEY CONCEPT:
 *   Eliminates the repeated XrayIsolateParams boilerplate scattered
 *   across run/repl/test/check/fmt/compile/deps/eval.
 *   Each profile selects appropriate init_flags; callers override
 *   only what they need (JIT, trace, workers, etc.).
 */

#include "xcli_isolate.h"
#include "xcli_diag.h"
#include "../../base/xchecks.h"
#include <stdio.h>
#include <string.h>

/* ========== Profile Configuration ========== */

void xr_cli_isolate_params(XrCliIsolateProfile profile,
                            XrayIsolateParams *out) {
    XR_DCHECK(out != NULL, "out must not be NULL");

    /* Start with defaults */
    xray_isolate_params_init(out);

    switch (profile) {
    case XR_CLI_ISOLATE_RUN:
    case XR_CLI_ISOLATE_EVAL:
    case XR_CLI_ISOLATE_REPL:
    case XR_CLI_ISOLATE_TEST:
        /* Full runtime: all subsystems */
        xray_isolate_setup_full(out);
        break;

    case XR_CLI_ISOLATE_PARSE:
        /* Minimal: just compiler + source cache */
        out->init_flags = XR_INIT_VM | XR_INIT_GC | XR_INIT_COMPILER |
                          XR_INIT_SOURCE_CACHE;
        xray_isolate_setup_full(out);
        break;

    case XR_CLI_ISOLATE_ANALYZE:
        /* Compiler + analyzer + source cache */
        out->init_flags = XR_INIT_VM | XR_INIT_GC | XR_INIT_COMPILER |
                          XR_INIT_ANALYZER | XR_INIT_SOURCE_CACHE |
                          XR_INIT_CLASSES | XR_INIT_SYMBOLS;
        xray_isolate_setup_full(out);
        break;
    }
}

/* ========== Create ========== */

XrayIsolate *xr_cli_isolate_create(const XrayIsolateParams *params) {
    XR_DCHECK(params != NULL, "params must not be NULL");

    XrayIsolate *iso = xray_isolate_new(params);
    if (!iso) {
        fprintf(stderr, "xray: failed to create isolate\n");
    }
    return iso;
}

XrayIsolate *xr_cli_isolate_new(XrCliIsolateProfile profile) {
    XrayIsolateParams params;
    xr_cli_isolate_params(profile, &params);
    return xr_cli_isolate_create(&params);
}
