/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_prepare_internal.h - Ordinary admission before Source lifetime work
 */
#ifndef XXIR_VIEW_PREPARE_INTERNAL_H
#define XXIR_VIEW_PREPARE_INTERNAL_H
#include "xxir_construction.h"
/* Reuses the complete ordinary module/type/declaration/function/SSA prefix of
 * the production verifier, including cleanup, panic and initialization rules.
 * No Artifact, effects, Cell provenance, borrowed permission or lifetime proof
 * is published. The producer must keep this generation immutable during its
 * analysis and repeat full admission after any rewrite. */
XR_FUNC XrXirStatus xir_compile_view_prepare_ordinary(const XrXirCompileContext *context,
    const XrXirModule *module,const XrXirConstruction *construction,XrXirDiagnostic *diagnostic);
#endif // XXIR_VIEW_PREPARE_INTERNAL_H
