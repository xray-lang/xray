/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_instance_internal.h - Ordinary Source receiving of real instances
 *
 * KEY CONCEPT:
 *   Actual closed instances execute before a fresh Source owner records data.
 *   New functions retain genuine declaration ancestry and undergo full checking.
 */
#ifndef XXIR_SOURCE_INSTANCE_INTERNAL_H
#define XXIR_SOURCE_INSTANCE_INTERNAL_H
#include "xxir_source.h"
#include "xxir_ctfe_instance_internal.h"

typedef struct XirSourceInstanceSelection {
    uint32_t definition,function;
} XirSourceInstanceSelection;
typedef struct XirSourceInstanceRequest {
    const XrXirSourceRequest *source;
    const XrXirSourceText *text;
    const XrModuleOverlayInput *overlays;
    size_t overlay_count;
    const XirSourceInstanceSelection *selections;
    uint32_t count;
    XrXirTarget target;
    XrXirCtfeLimits limits;
    uint32_t call_depth;
} XirSourceInstanceRequest;
typedef struct XirSourceInstanceConstants {
    XrXirSourceResult source;
    uint32_t *functions;
    uint32_t count;
} XirSourceInstanceConstants;

/* Selection pairs identify this actual Source definition and collected instance.
 * Only instances already collected by ordinary specialization are selectable.
 * Each result is freshly executed and replayed by the ordinary Source writer;
 * original generic bodies are not revisited for each closed substitution.
 * No Source-language expression replacement or future Library binding follows.
 * All owners and input buffers remain immutable throughout this synchronous call.
 * Failure preserves a zero output; work and allocations are never refunded. */
XR_FUNC XrXirStatus xr_xir_compile_source_instance_constants(
    const XirSourceInstanceRequest *request,XirSourceInstanceConstants *output,
    XrXirSourceDiagnostic *diagnostic);
XR_FUNC void xr_xir_compile_source_instance_constants_free(XirSourceInstanceConstants *result);
#endif // XXIR_SOURCE_INSTANCE_INTERNAL_H
