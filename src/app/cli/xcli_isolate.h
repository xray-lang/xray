/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_isolate.h - Profile-based isolate creation for CLI commands
 *
 * KEY CONCEPT:
 *   Replaces scattered XrayIsolateParams setup across subcommands with
 *   a single factory function.  Each profile sets appropriate init_flags
 *   and defaults.  Callers can then override individual fields (JIT,
 *   trace, workers, etc.) before calling xr_cli_isolate_create().
 */

#ifndef XCLI_ISOLATE_H
#define XCLI_ISOLATE_H

#include "../../base/xdefs.h"
#include "xray_isolate.h"

/* ========== Isolate Profiles ========== */

typedef enum {
    XR_CLI_ISOLATE_RUN,     /* Full runtime: VM + GC + compiler + stdlib + modules */
    XR_CLI_ISOLATE_EVAL,    /* Same as RUN but for eval/stdin */
    XR_CLI_ISOLATE_PARSE,   /* Parse only: compiler + source cache (no stdlib) */
    XR_CLI_ISOLATE_ANALYZE, /* Check/fmt: compiler + analyzer + source cache */
    XR_CLI_ISOLATE_TEST,    /* Test: full runtime (JIT configurable) */
    XR_CLI_ISOLATE_REPL,    /* REPL: full runtime + source cache */
} XrCliIsolateProfile;

/* Initialize params for the given profile.
 * Caller can then override fields before calling xr_cli_isolate_create(). */
XR_FUNC void xr_cli_isolate_params(XrCliIsolateProfile profile, XrayIsolateParams *out);

/* Create isolate from pre-configured params.
 * Thin wrapper: calls xray_isolate_new and logs error on failure.
 * Returns NULL on failure. */
XR_FUNC XrayIsolate *xr_cli_isolate_create(const XrayIsolateParams *params);

/* Convenience: init params for profile + create in one call.
 * Equivalent to xr_cli_isolate_params + xr_cli_isolate_create.
 * For simple cases where no override is needed. */
XR_FUNC XrayIsolate *xr_cli_isolate_new(XrCliIsolateProfile profile);

#endif  // XCLI_ISOLATE_H
