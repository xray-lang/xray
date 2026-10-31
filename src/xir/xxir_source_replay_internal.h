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
XR_FUNC XrXirStatus xir_effects_raw_source_terminal_input(const XrXirCompileContext *context,
    XirEffectInput input,const XrXirEffects *effects);
XR_FUNC XrXirStatus xir_compile_verify_receiving_collect(const XrXirCompileContext *context,
    const XrXirModule *module,const XrXirConstruction *construction,
    const XrXirEffects *source_effects,XrXirEffects **output,XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xir_compile_verify_receiving_replay(const XrXirCompileContext *context,
    const XrXirModule *module,const XrXirConstruction *construction,
    const XrXirEffects *source_effects,const XrXirEffects *effects,XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xir_effects_receiving_terminal_input(const XrXirCompileContext *context, XirEffectInput input,const XrXirEffects *effects);
XR_FUNC XrXirStatus xir_compile_verify_built_collect(const XrXirCompileContext *context,
    const XrXirModule *module,const XrXirConstruction *construction,
    XrXirEffects **output,XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xir_compile_verify_built_replay(const XrXirCompileContext *context,
    const XrXirModule *module,const XrXirConstruction *construction,
    const XrXirEffects *effects,XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xir_effects_built_copy_match_input(const XrXirCompileContext *context, XirEffectInput input,const XrXirEffects *effects);
XR_FUNC XrXirStatus xir_effects_built_copy_terminal_input(const XrXirCompileContext *context, XirEffectInput input,const XrXirEffects *effects);
#endif // XXIR_SOURCE_REPLAY_INTERNAL_H
