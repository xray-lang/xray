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
    const char *prefix;
    size_t code_limit;
} XrXirNativeProjectionRequest;

typedef struct XrXirNativeProjection XrXirNativeProjection;
typedef struct XrXirNativeProjectionSource {
    const char *text;
    size_t length;
} XrXirNativeProjectionSource;
typedef struct XrXirNativeProjectionFacts {
    XrXirSourceProductFacts source;
    const char *prefix;
    XrFingerprint codegen_policy_id, generated_digest;
} XrXirNativeProjectionFacts;

/* Output must be NULL and is preserved on failure. One ordinary product verify
 * and one actual emission produce owned C and copied facts on the original
 * ledger. The product and its context may be destroyed after this returns. */
XR_FUNC XrXirStatus xr_compile_native_projection_prepare(const XrXirNativeProjectionRequest *request,
    XrXirNativeProjection **output, XrXirDiagnostic *diagnostic);
/* Immutable borrows last until owner_free. Queries consume no work. */
XR_FUNC const XrXirCompileContext *xr_compile_native_projection_context(const XrXirNativeProjection *owner);
XR_FUNC const XrXirNativeProjectionSource *xr_compile_native_projection_source(const XrXirNativeProjection *owner);
XR_FUNC const XrXirNativeProjectionFacts *xr_compile_native_projection_facts(const XrXirNativeProjection *owner);
/* Bind already independently validated toolchain facts without allocating,
 * verifying the product again or emitting C again. This charges the owner's
 * original ledger. Failure preserves all output bytes and the immutable owner;
 * the identity alone grants no SDK, Target or execution authority. */
XR_FUNC XrXirStatus xr_compile_native_projection_bind(const XrXirNativeProjection *owner,
    const XrToolchainBinding *binding, XrXirNativeInput *output);
XR_FUNC void xr_compile_native_projection_owner_free(XrXirNativeProjection *owner);
#endif // XR_XIR_NATIVE_PROJECTION_H
