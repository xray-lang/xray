/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_control_input_terminal_cases.h - Actual Input control seed counterfacts
 */
#ifndef XIR_CONTROL_INPUT_TERMINAL_CASES_H
#define XIR_CONTROL_INPUT_TERMINAL_CASES_H
static void control_current_opcode_cases(void) {
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    PcControlFixture fixture;pc_control_fixture(&fixture,true);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xD7,sizeof(fixture));
    EffectBodyMaterial *body=(EffectBodyMaterial *)effects->invocations->body;
    EffectFinalOutcomes *final=effects->invocations->final->outcomes;
    control_verify(&context,effects,XR_XIR_OK);
    for(uint32_t f=1;f<3;++f) {
        EffectBodyRun *run=&body->functions[f].runs[0];
        CHECK(!run->first && run->count && run->base.op==XR_XIR_SUSPEND);
        CHECK(final->functions[f].suspend==XR_XIR_EFFECT_MAY);
        run->base.op=XR_XIR_RETURN;control_verify(&context,effects,XR_XIR_BAD_STRUCTURE);
        run->base.op=XR_XIR_INVALID;control_verify(&context,effects,XR_XIR_BAD_STRUCTURE);
        run->base.op=XR_XIR_SUSPEND;control_verify(&context,effects,XR_XIR_OK);
        final->functions[f].suspend=XR_XIR_EFFECT_NONE;
        control_verify(&context,effects,XR_XIR_BAD_STRUCTURE);
        final->functions[f].suspend=XR_XIR_EFFECT_MAY;control_verify(&context,effects,XR_XIR_OK);
    }
    xr_xir_compile_effects_free(effects);pc_release(&context,baseline);
}
static void control_current_signature_case(bool no_suspend) {
    XrXirTypeNode node={.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_UNIT,
        .flags=XR_XIR_CALLABLE_ROOT_NONE|(no_suspend?XR_XIR_CALLABLE_NO_SUSPEND:0)};
    XrXirTypes types={.nodes=&node,.count=1};XrXirType parameter=XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirInstruction ops[2]={{.op=XR_XIR_CALL_INDIRECT,.immediate=0},{.op=XR_XIR_RETURN}};
    XrXirBlock block={.count=2};
    XrXirFunction function={.name="f",.name_length=1,.parameters=&parameter,.parameter_count=1,
        .instructions=ops,.instruction_count=2,.blocks=&block,.block_count=1};
    XrXirModule module={.stage=XR_XIR_BUILT,.types=&types,.functions=&function,.function_count=1};
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);memset(&node,0xD8,sizeof(node));memset(ops,0xD8,sizeof(ops));
    memset(&function,0xD8,sizeof(function));parameter=XR_XIR_UNIT;
    EffectBodyMaterial *body=(EffectBodyMaterial *)effects->invocations->body;
    const EffectFinalOutcomes *final=effects->invocations->final->outcomes;
    CHECK(final->functions[0].suspend==(no_suspend?XR_XIR_EFFECT_NONE:XR_XIR_EFFECT_UNKNOWN));
    CHECK(final->witnesses[0].cause==(no_suspend?XR_XIR_EFFECT_CAUSE_NONE:XR_XIR_EFFECT_CAUSE_INDIRECT));
    control_verify(&context,effects,XR_XIR_OK);
    XrXirTypeNode *actual=(XrXirTypeNode *)&body->terms.types.nodes[0];uint32_t flags=actual->flags;
    actual->flags^=XR_XIR_CALLABLE_NO_SUSPEND;control_verify(&context,effects,XR_XIR_BAD_STRUCTURE);
    actual->flags=flags;control_verify(&context,effects,XR_XIR_OK);
    XrXirType *formal=(XrXirType *)body->functions[0].parameters;XrXirType saved=*formal;
    *formal=XR_XIR_UNIT;control_verify(&context,effects,XR_XIR_BAD_STRUCTURE);
    *formal=saved;control_verify(&context,effects,XR_XIR_OK);
    xr_xir_compile_effects_free(effects);pc_release(&context,baseline);
}
static XrXirTypeNode *control_late_source,*control_late_input;
static unsigned control_late_callbacks;
static void control_late_signature(void *memory,size_t bytes) {
    (void)memory;(void)bytes;pc_allocate_hook=NULL;++control_late_callbacks;
    control_late_source->flags^=XR_XIR_CALLABLE_NO_SUSPEND;
    control_late_input->flags^=XR_XIR_CALLABLE_NO_SUSPEND;
}
static void control_late_signature_case(bool no_suspend) {
    XrXirTypeNode node={.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_UNIT,
        .flags=XR_XIR_CALLABLE_ROOT_NONE|(no_suspend?XR_XIR_CALLABLE_NO_SUSPEND:0)};
    XrXirTypes types={.nodes=&node,.count=1};XrXirType parameter=XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirInstruction ops[2]={{.op=XR_XIR_CALL_INDIRECT,.immediate=0},{.op=XR_XIR_RETURN}};
    XrXirBlock block={.count=2};
    XrXirFunction function={.name="f",.name_length=1,.parameters=&parameter,.parameter_count=1,
        .instructions=ops,.instruction_count=2,.blocks=&block,.block_count=1};
    XrXirModule module={.stage=XR_XIR_BUILT,.types=&types,.functions=&function,.function_count=1};
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&module,&checked,NULL)==XR_XIR_OK);
    uint64_t held=pc_stats(&context).live_bytes;const XrXirModule *source=xr_xir_compile_artifact_module(checked);
    CHECK(source && effect_infer_construct(&context,source,NULL,&effects)==XR_XIR_OK);
    const EffectBodyMaterial *body=effects->invocations->body;
    control_late_source=(XrXirTypeNode *)&source->types->nodes[0];
    control_late_input=(XrXirTypeNode *)&body->terms.types.nodes[0];
    uint32_t raw_flags=control_late_source->flags,owned_flags=control_late_input->flags;
    control_late_callbacks=0;pc_allocate_hook=control_late_signature;
    XrXirStatus status=effect_outcomes_publish(&context,source,effects);pc_allocate_hook=NULL;
    CHECK(status==XR_XIR_BAD_STRUCTURE && control_late_callbacks==1 && !effects->invocations->final->outcomes);
    CHECK(effect_invocation_current_input_match(&context,source,body)==XR_XIR_OK);
    control_late_source->flags=raw_flags;control_late_input->flags=owned_flags;
    control_late_source=NULL;control_late_input=NULL;
    xr_xir_compile_effects_free(effects);effects=NULL;CHECK(pc_stats(&context).live_bytes==held);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    control_verify(&context,effects,XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);xr_xir_compile_effects_free(effects);pc_release(&context,baseline);
}
static void control_current_input_cases(void) {
    control_current_opcode_cases();control_current_signature_case(false);control_current_signature_case(true);
    control_late_signature_case(false);control_late_signature_case(true);
    puts("Control actual Input: opcode/signature/formal mutations; NONE/UNKNOWN/MAY; producer death; no callbacks; late matching Source/Input mutation rejected; retry; physical0");
}
#endif // XIR_CONTROL_INPUT_TERMINAL_CASES_H
