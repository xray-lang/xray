/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_emit_c.h - Bounded C11 emission from verified Lowered XIR
 *
 * KEY CONCEPT:
 *   Only complete, structurally verified translation units leave the emitter.
 */

#ifndef XXIR_EMIT_C_H
#define XXIR_EMIT_C_H

#include "xxir.h"

typedef struct XrXirCSource { char *text; size_t length; } XrXirCSource;

XR_FUNC XrXirStatus xr_xir_emit_c(const XrXirArtifact *artifact, const char *symbol_prefix,
                                size_t byte_limit, XrXirCSource *output);
XR_FUNC void xr_xir_c_source_free(XrXirCSource *source);

#endif // XXIR_EMIT_C_H
