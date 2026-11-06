/* Real test construction for raw compiler-owned fixtures. The raw Module
 * remains unchanged; the receiver retains a separate owned fact generation. */
#ifndef XIR_CONDITIONAL_EFFECT_INPUT_H
#define XIR_CONDITIONAL_EFFECT_INPUT_H
#include "xir_construction_fixture.h"
static inline XrXirStatus conditional_facts_from_raw(const XrXirCompileContext *context,
    const XrXirModule *module,EffectInputFacts **output) {
    if (!output || *output || xir_fixture_invalid_prefix(context,module)) return XR_XIR_BAD_STRUCTURE;
    XrXirConstruction *construction=NULL;
    XrXirStatus status=xir_fixture_module_construction(context,module,&construction);
    if (status==XR_XIR_OK) status=effect_input_facts_new(context,
        (XirEffectInput){module,construction,NULL},output);
    xr_xir_compile_construction_free(construction);return status;
}
static inline XrXirStatus conditional_owned_body_from_raw(const XrXirCompileContext *context,
    const XrXirModule *module,EffectBodyMaterial **output) {
    if (!output || *output || xir_fixture_invalid_prefix(context,module)) return XR_XIR_BAD_STRUCTURE;
    XrXirConstruction *construction=NULL;EffectInputFacts *facts=NULL;
    XrXirStatus status=xir_fixture_module_construction(context,module,&construction);
    if (status==XR_XIR_OK) status=effect_input_facts_new(context,(XirEffectInput){module,construction,NULL},&facts);
    if (status==XR_XIR_OK) status=effect_body_material_create(context,module,facts,output);
    effect_input_facts_free(facts);xr_xir_compile_construction_free(construction);return status;
}
static inline XrXirStatus conditional_effects_from_raw(const XrXirCompileContext *context,
    const XrXirModule *module,XrXirEffects **output) {
    if (!output || *output || xir_fixture_invalid_prefix(context,module)) return XR_XIR_BAD_STRUCTURE;
    XrXirConstruction *construction=NULL;
    XrXirStatus status=xir_fixture_module_construction(context,module,&construction);
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_infer_verified_input(context,
        (XirEffectInput){module,construction,NULL},output);
    xr_xir_compile_construction_free(construction);return status;
}
static inline XrXirStatus conditional_context_from_input(const XrXirCompileContext *context,
    XirEffectInput input,EffectRefinementBounds *bounds,EffectBodyMaterial *initial,
    EffectContextOwner **output) {
    if (!output || *output) return XR_XIR_BAD_STRUCTURE;
    EffectInputFacts *facts=NULL;
    XrXirStatus status=effect_input_facts_new(context,input,&facts);
    if (status==XR_XIR_OK) status=effect_context_owner_build(context,input.module,bounds,initial,facts,output);
    effect_input_facts_free(facts);return status;
}
static inline XrXirStatus conditional_context_from_raw(const XrXirCompileContext *context,
    const XrXirModule *module,EffectRefinementBounds *bounds,EffectBodyMaterial *initial,
    EffectContextOwner **output) {
    if (!output || *output || xir_fixture_invalid_prefix(context,module)) return XR_XIR_BAD_STRUCTURE;
    EffectInputFacts *facts=NULL;
    XrXirStatus status=conditional_facts_from_raw(context,module,&facts);
    if (status==XR_XIR_OK) status=effect_context_owner_build(context,module,bounds,initial,facts,output);
    effect_input_facts_free(facts);return status;
}
static inline XrXirStatus conditional_ordinary_from_raw(const XrXirCompileContext *context,
    const XrXirModule *module,EffectBodyMaterial *initial,EffectRefinementBounds *bounds,
    EffectOrdinaryContexts **output) {
    if (!output || *output || xir_fixture_invalid_prefix(context,module)) return XR_XIR_BAD_STRUCTURE;
    EffectInputFacts *facts=NULL;
    XrXirStatus status=conditional_facts_from_raw(context,module,&facts);
    if (status==XR_XIR_OK) status=effect_ordinary_build(context,module,initial,bounds,facts,output);
    effect_input_facts_free(facts);return status;
}
static inline XrXirStatus conditional_capture_from_raw(const XrXirCompileContext *context,
    const XrXirModule *module,XrXirEffects *effects) {
    if (xir_fixture_invalid_prefix(context,module) || !effects || effects->refinement ||
        !xir_effects_context_matches(context,effects,module->function_count) || effect_contexts_read(effects))
        return XR_XIR_BAD_STRUCTURE;
    XrXirConstruction *construction=NULL;EffectInputFacts *facts=NULL;
    XrXirStatus status=xir_fixture_module_construction(context,module,&construction);
    if (status==XR_XIR_OK) status=effect_input_facts_new(context,
        (XirEffectInput){module,construction,NULL},&facts);
    XrXirEffects receiving=*effects;receiving.input_facts=facts;
    if (status==XR_XIR_OK) status=xir_effects_refinement_capture_input(context,
        (XirEffectInput){module,construction,NULL},&receiving);
    if (status==XR_XIR_OK) effects->refinement=receiving.refinement;
    else effect_refinement_free(receiving.refinement);
    effect_input_facts_free(facts);xr_xir_compile_construction_free(construction);return status;
}
#if defined(XR_XIR_EFFECT_CONTEXT_TESTS)
static inline XrXirStatus conditional_classify_from_raw(const XrXirCompileContext *context,
    const XrXirModule *module,const EffectContextResolved *seed,
    EffectOrdinaryContexts **output,uint32_t *selected) {
    if (!output || *output || !selected || xir_fixture_invalid_prefix(context,module)) return XR_XIR_BAD_STRUCTURE;
    EffectInputFacts *facts=NULL;
    XrXirStatus status=conditional_facts_from_raw(context,module,&facts);
    if (status==XR_XIR_OK) status=effect_ordinary_classify(context,module,seed,facts,output,selected);
    effect_input_facts_free(facts);return status;
}
#endif
#endif
