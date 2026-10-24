/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_effect_input_fixture.h - Explicit borrowed compiler inputs for tests
 *
 * KEY CONCEPT:
 *   A pair preserves the original Module address and borrows its live facts.
 *   Raw forwards perform no validation, allocation or implicit retention.
 */
#ifndef XIR_EFFECT_INPUT_FIXTURE_H
#define XIR_EFFECT_INPUT_FIXTURE_H
#include "xir/xxir_internal.h"
#include "xir/xxir_effect_contract_internal.h"

typedef struct XirFixtureEffectInput {
    const XrXirModule *module;
    const XrXirConstruction *construction;
} XirFixtureEffectInput;

/* Current raw entries consume only Module. The explicit Construction borrow
 * is carried here but is neither consumed nor verified by those entries. */
static inline XrXirStatus xir_fixture_effects_infer(const XrXirCompileContext *context,
    XirFixtureEffectInput input,XrXirEffects **output) {
    (void)input.construction;
    return xr_xir_compile_effects_infer_verified(context,input.module,output);
}
static inline XrXirStatus xir_fixture_effects_body_match(const XrXirCompileContext *context,
    XirFixtureEffectInput input,const XrXirEffects *effects) {
    (void)input.construction;
    return xir_effects_source_body_match(context,input.module,effects);
}
static inline XrXirStatus xir_fixture_effects_program_body(const XrXirCompileContext *context,
    XirFixtureEffectInput input,const XrXirEffects *effects) {
    (void)input.construction;
    return xir_effects_program_body_match(context,input.module,effects);
}
static inline XrXirStatus xir_fixture_effects_program_final(const XrXirCompileContext *context,
    XirFixtureEffectInput input,const XrXirEffects *effects) {
    (void)input.construction;
    return xir_effects_program_final_verify(context,input.module,effects);
}
static inline XrXirStatus xir_fixture_effects_program_root(const XrXirCompileContext *context,
    XirFixtureEffectInput input,const XrXirEffects *effects) {
    (void)input.construction;
    return xir_effects_program_root_current(context,input.module,effects);
}
static inline XrXirStatus xir_fixture_effects_refine(const XrXirCompileContext *context,
    XirFixtureEffectInput input,const XrXirRootRefiner *refiner,XrXirEffects **output) {
    (void)input.construction;
    return xr_xir_compile_effects_refine_verified(context,input.module,refiner,output);
}
static inline XrXirStatus xir_fixture_effects_refresh(const XrXirCompileContext *context,
    XirFixtureEffectInput input,const XrXirEffects *effects) {
    (void)input.construction;
    return xir_effects_context_refresh(context,input.module,effects);
}
static inline XrXirStatus xir_fixture_effects_capture(const XrXirCompileContext *context,
    XirFixtureEffectInput input,const XrXirEffects *effects) {
    (void)input.construction;
    return xir_effects_refinement_capture(context,input.module,effects);
}
static inline XrXirStatus xir_fixture_effects_terminal(const XrXirCompileContext *context,
    XirFixtureEffectInput input,const XrXirEffects *effects) {
    (void)input.construction;
    return xir_effects_source_terminal(context,input.module,effects);
}
#endif // XIR_EFFECT_INPUT_FIXTURE_H
