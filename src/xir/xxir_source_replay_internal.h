/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_replay_internal.h - Lexical raw source derivation ownership
 *
 * KEY CONCEPT:
 *   The owning operation keeps complete evidence, never a passed verdict.
 *   Every consumer proves its actual current input and all finite relations.
 */
#ifndef XXIR_SOURCE_REPLAY_INTERNAL_H
#define XXIR_SOURCE_REPLAY_INTERNAL_H
#include "xxir_internal.h"
XR_FUNC XrXirStatus xir_compile_verify_raw_source_effects(const XrXirCompileContext *context,
    const XrXirModule *source,const XrXirConstruction *construction,
    const XrXirEffects *effects,XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xir_artifact_verify_raw_source_effects(const XrXirArtifact *artifact,
    XrXirEffects **output,XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xir_effects_raw_source_terminal(const XrXirCompileContext *context,
    const XrXirModule *source,const XrXirEffects *effects);
#endif // XXIR_SOURCE_REPLAY_INTERNAL_H
