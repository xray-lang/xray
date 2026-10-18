/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_comptime_resolve_internal.h - Complete Source keys through genuine CTFE
 *
 * KEY CONCEPT:
 *   Missing actual keys select compiler-only helpers, never caller values or
 *   runtime fallback. Original definitions and existing providers retain meaning.
 */
#ifndef XXIR_COMPTIME_RESOLVE_INTERNAL_H
#define XXIR_COMPTIME_RESOLVE_INTERNAL_H
#include "xxir_ctfe.h"
#include "xxir_construction.h"
typedef struct XirComptimeResolveRequest {
    const XrXirArtifact *definition;
    XrXirTarget target;
    XrXirCtfeLimits limits;
    uint32_t call_depth;
} XirComptimeResolveRequest;
typedef struct XirComptimeResolveFailure {
    XrXirCtfeStatus status;
    XrXirComptimeSite site;
    bool root_local;
} XirComptimeResolveFailure;
/* The immutable input stays alive throughout this synchronous operation.
 * Output and optional failure metadata start empty. The result independently
 * owns genuine Source with every actually required key, original TEMPLATE and
 * declarations. Uninstantiated sites require no evaluation. Every allocation,
 * current proof and retry uses the original finite compiler ledger. */
XR_FUNC XrXirStatus xr_xir_compile_comptime_resolve(const XirComptimeResolveRequest *request,
    XrXirArtifact **output,XirComptimeResolveFailure *failure,XrXirDiagnostic *diagnostic);
#endif // XXIR_COMPTIME_RESOLVE_INTERNAL_H
