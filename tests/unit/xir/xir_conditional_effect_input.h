/* Real test construction for raw compiler-owned fixtures. The raw Module
 * remains unchanged; the receiver retains a separate owned fact generation. */
#ifndef XIR_CONDITIONAL_EFFECT_INPUT_H
#define XIR_CONDITIONAL_EFFECT_INPUT_H
#include "xir_construction_fixture.h"
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
#endif
