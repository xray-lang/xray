/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_backend_emission_owner.h - Finite caller ledger for retiring generated-C tests
 *
 * KEY CONCEPT:
 *   One main owns the verifier ledger through every repeated emission.
 */
#ifndef XR_BACKEND_EMISSION_OWNER_H
#define XR_BACKEND_EMISSION_OWNER_H
#include "base/xcompile_resources.h"
#include <stdio.h>
#include <stdlib.h>
static XrCompileResources *backend_emission_resources;
static XrCompileResourceStats backend_emission_initial;
static void backend_emission_owner_close(void) {
    XrCompileResourceStats stats;
    if (!backend_emission_resources)
        return;
    if (xr_compile_resources_stats(backend_emission_resources, &stats) != XR_COMPILE_RESOURCE_OK ||
        stats.live_bytes != backend_emission_initial.live_bytes) {
        fputs("backend emission scratch was not released\n", stderr);
        abort();
    }
    xr_compile_resources_release(backend_emission_resources);
    backend_emission_resources = NULL;
}
static bool backend_emission_owner_open(XrCompileResourceLimits limits) {
    if (backend_emission_resources ||
        xr_compile_resources_new(&limits, &backend_emission_resources) != XR_COMPILE_RESOURCE_OK)
        return false;
    if (xr_compile_resources_stats(backend_emission_resources, &backend_emission_initial) !=
            XR_COMPILE_RESOURCE_OK ||
        atexit(backend_emission_owner_close) != 0) {
        xr_compile_resources_release(backend_emission_resources);
        backend_emission_resources = NULL;
        return false;
    }
    return true;
}
#endif  // XR_BACKEND_EMISSION_OWNER_H
