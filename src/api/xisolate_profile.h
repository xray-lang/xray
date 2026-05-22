/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xisolate_profile.h - Profile-based isolate creation
 *
 * KEY CONCEPT:
 *   Replaces scattered XrayIsolateParams setup across CLI subcommands and
 *   MCP server with a single factory.  Each profile sets appropriate
 *   init_flags and runtime defaults; callers can override individual fields
 *   (JIT, trace, workers) before calling xr_isolate_profile_create().
 *
 *   Lives in src/api/ so CLI, MCP, LSP, DAP and embedders share one
 *   authoritative isolate factory and avoid sibling-app cross-dependencies.
 */

#ifndef XISOLATE_PROFILE_H
#define XISOLATE_PROFILE_H

#include "../base/xdefs.h"
#include "xray_isolate.h"

/* ========== Isolate Profiles ========== */

typedef enum {
    XR_ISOLATE_PROFILE_RUN,     /* Full runtime: VM + GC + compiler + stdlib + modules */
    XR_ISOLATE_PROFILE_EVAL,    /* Same as RUN but for eval/stdin */
    XR_ISOLATE_PROFILE_PARSE,   /* Parse only: compiler + source cache (no stdlib) */
    XR_ISOLATE_PROFILE_ANALYZE, /* Check/fmt: compiler + analyzer + source cache */
    XR_ISOLATE_PROFILE_TEST,    /* Test: full runtime (JIT configurable) */
    XR_ISOLATE_PROFILE_REPL,    /* REPL: full runtime + source cache */
} XrIsolateProfile;

/* Initialize params for the given profile.
 * Caller can then override fields before calling xr_isolate_profile_create(). */
XR_FUNC void xr_isolate_profile_params(XrIsolateProfile profile, XrayIsolateParams *out);

/* Create isolate from pre-configured params.
 * Thin wrapper: calls xray_isolate_new and logs error on failure.
 * Returns NULL on failure. */
XR_FUNC XrayIsolate *xr_isolate_profile_create(const XrayIsolateParams *params);

/* Convenience: init params for profile + create in one call.
 * Equivalent to xr_isolate_profile_params + xr_isolate_profile_create.
 * For simple cases where no override is needed. */
XR_FUNC XrayIsolate *xr_isolate_profile_new(XrIsolateProfile profile);

#endif  // XISOLATE_PROFILE_H
