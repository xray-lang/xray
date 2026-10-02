/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xir_native_projection.h - Actual source and C identities on one ledger
 */
#ifndef XR_XIR_NATIVE_PROJECTION_H
#define XR_XIR_NATIVE_PROJECTION_H
#include "xr_xir_native_artifact.h"
#include "../../program/xr_xir_source_product.h"

typedef struct XrXirNativeProjectionRequest {
    const XrXirSourceProduct *product;
    const XrToolchainBinding *toolchain;
    const char *prefix;
    size_t code_limit;
} XrXirNativeProjectionRequest;

typedef struct XrXirNativeProjection {
    XrXirNativeInput input;
    XrXirCSource source;
} XrXirNativeProjection;

/* The output must be empty. Failure preserves every output byte. All work and
 * allocation use the product's retained context. Source and target authority
 * remain separate: a binding alone does not authorize native execution. */
XR_FUNC XrXirStatus xr_compile_native_projection_build(const XrXirNativeProjectionRequest *request,
    XrXirNativeProjection *output, XrXirDiagnostic *diagnostic);
XR_FUNC void xr_compile_native_projection_free(XrXirNativeProjection *projection);
#endif // XR_XIR_NATIVE_PROJECTION_H
