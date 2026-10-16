/* Real Lowered inputs exercise the current independent receiving kernels. */
#define main storage_fixture_entry
#include "test_xir_program_input_storage.c"
#undef main

static void rejected_final(const XrXirCompileContext *context,XrXirArtifact *artifact,
    XrXirEffects *effects) {
    EffectFinalOutcomes *final=(EffectFinalOutcomes *)effect_final_outcomes(effects);
    CHECK(final && effects->invocations->parameter_kinds[2]);
    XrXirRootEffects root=final->root[1];
    XrXirFunctionEffects control=final->functions[1];
    XrXirStatus worker=final->go_safe[1],errors=final->task_errors[1];
    uint32_t intrinsic=final->entry_roots[1].intrinsic_mask;
    XrXirEffectParameter *parameter=(XrXirEffectParameter *)final->contracts[2].parameters;
    XrXirEffectParameter saved=*parameter;
    uint8_t kind=effects->invocations->parameter_kinds[2][0];
    for(unsigned attack=0;attack<6;++attack) {
        if(attack==0)final->root[1].requires_root=true;
        if(attack==1)final->functions[1].suspend=true;
        if(attack==2)final->go_safe[1]=XR_XIR_BAD_TYPE;
        if(attack==3)final->task_errors[1]=XR_XIR_BAD_TYPE;
        if(attack==4)final->entry_roots[1].intrinsic_mask=XR_XIR_CALLABLE_ROOT_REQUIRED;
        if(attack==5) {
            parameter->kind=XR_XIR_EFFECT_PARAMETER_VARIABLE;
            effects->invocations->parameter_kinds[2][0]=XR_XIR_EFFECT_PARAMETER_VARIABLE;
        }
        XrXirStatus status=xir_effects_program_final_verify(context,&artifact->module,effects);
        fprintf(stderr,"received Final attack%u status%u\n",attack,status);
        final->root[1]=root;final->functions[1]=control;final->go_safe[1]=worker;
        final->task_errors[1]=errors;final->entry_roots[1].intrinsic_mask=intrinsic;
        *parameter=saved;effects->invocations->parameter_kinds[2][0]=kind;
        CHECK(status==XR_XIR_BAD_STRUCTURE);
        CHECK(xir_effects_program_final_verify(context,&artifact->module,effects)==XR_XIR_OK);
    }
}

int main(void) {
    const XrCompileResourceLimits limits={67108864,8388608,128000000};
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *artifact=program_input(&context);
    XrXirEffects *effects=NULL;
    CHECK(xr_xir_compile_effects_analyze(artifact,&effects)==XR_XIR_OK);
    const EffectInvocationCertificate *certificate=effects->invocations;
    fprintf(stderr,"stage=%u certificate_remaining=%p body_remaining=%p contexts=%p\n",
        artifact->module.stage,(void *)certificate->terms.remaining,
        (void *)certificate->body->terms.remaining,(void *)effect_contexts_read(effects));
    XrXirStatus outcomes[6];
    outcomes[0]=effect_source_certificate(&context,certificate,effects);
    fprintf(stderr,"independent_certificate=%u\n",outcomes[0]);
    outcomes[1]=xir_effects_program_final_verify(&context,&artifact->module,effects);
    fprintf(stderr,"current_lowered_receiver=%u\n",outcomes[1]);
    outcomes[2]=effect_source_publication_match(&context,&artifact->module,effects);
    fprintf(stderr,"public_projection=%u\n",outcomes[2]);
    outcomes[3]=effect_source_outcomes(&context,effects);
    fprintf(stderr,"independent_control_error=%u\n",outcomes[3]);
    outcomes[4]=effect_invocation_current_input_match(&context,&artifact->module,certificate->body);
    fprintf(stderr,"current_input=%u\n",outcomes[4]);
    outcomes[5]=xir_effects_program_body_match(&context,&artifact->module,effects);
    fprintf(stderr,"physical_correspondence=%u\n",outcomes[5]);
    CHECK(xir_effects_program_layout_match(&context,effects,&artifact->target,artifact->layouts)==XR_XIR_OK);
    uint32_t frame=artifact->layouts[1].frame_bytes;
    ++artifact->layouts[1].frame_bytes;
    CHECK(xir_effects_program_layout_match(&context,effects,&artifact->target,artifact->layouts)==XR_XIR_BAD_LAYOUT);
    artifact->layouts[1].frame_bytes=frame;
    const uint32_t *offsets=artifact->layouts[1].offsets;
    artifact->layouts[1].offsets=(const uint32_t *)(uintptr_t)1;
    CHECK(xir_effects_program_layout_match(&context,effects,&artifact->target,artifact->layouts)==XR_XIR_BAD_STRUCTURE);
    artifact->layouts[1].offsets=offsets;
    CHECK(xir_effects_program_layout_match(&context,effects,&artifact->target,artifact->layouts)==XR_XIR_OK);
    rejected_final(&context,artifact,effects);
    xr_xir_compile_effects_free(effects);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&artifact,&program)==XR_XIR_OK && !artifact);
    result42(program);xr_xir_compile_program_drop(program);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==baseline.live_bytes);
    xr_compile_resources_release(context.resources);
    for(unsigned i=0;i<6;++i) CHECK(outcomes[i]==XR_XIR_OK);
    puts("Lowered independent receiving kernels and producer-death result42 PASS");
    return 0;
}
