/* Current Input proves least suspension and the actual first shortest cause. */
#define main storage_fixture_entry
#include "test_xir_program_input_storage.c"
#undef main

static XrXirArtifact *suspension_program(const XrXirCompileContext *context,unsigned variant) {
    XrXirInstruction rows[5][3]={0};XrXirBlock blocks[5]={0};
    XrXirFunction functions[5]={0};XrXirFunctionIdentity identities[5]={0};
    for(uint32_t f=0;f<5;++f) {
        identities[f].method_kind=XR_XIR_NON_MEMBER;
        rows[f][0]=(XrXirInstruction){.op=XR_XIR_SUSPEND};
        rows[f][1]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
        rows[f][2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};
        blocks[f].count=3;
    }
    rows[0][0]=(XrXirInstruction){.op=XR_XIR_RETURN};blocks[0].count=1;
    rows[1][0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=2};
    rows[1][1]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=variant==1?2:3};
    rows[1][2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0}};
    if(variant==2) for(uint32_t f=2;f<4;++f) {
        rows[f][0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=4};
        rows[f][1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0}};blocks[f].count=2;
    }
    if(variant==3) {
        rows[1][1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0}};blocks[1].count=2;
        rows[2][0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=1};
        rows[2][1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0}};blocks[2].count=2;
    }
    for(uint32_t f=0;f<5;++f)
        functions[f]=(XrXirFunction){.name="f",.name_length=1,.result=f?XR_XIR_I64:XR_XIR_UNIT,
            .blocks=&blocks[f],.block_count=1,.instructions=rows[f],
            .instruction_count=blocks[f].count,.error_type=XR_XIR_NEVER};
    const XrXirSourceModule source={"root",4,NULL,0,0,0,XR_XIR_MODULE_FRESH_STORAGE};
    const XrXirDeclarations declarations={&source,1,identities,NULL,0,NULL,0,0,1,NULL};
    const XrXirModule module={XR_XIR_BUILT,functions,5,&declarations,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    const XrXirConstructionSpec spec={0};
    const XrXirSlotFacts slots={NULL,0};
    XrXirConstruction *construction=NULL;
    CHECK(xr_xir_compile_construction_new_v4(context,NULL,&spec,&slots,&construction)==XR_XIR_OK);
    XrXirDiagnostic diagnostic={0};
    XrXirStatus checked_status=xr_xir_compile_check_v2(context,&module,construction,&checked,&diagnostic);
    xr_xir_compile_construction_free(construction);
    if (checked_status!=XR_XIR_OK)
        fprintf(stderr,"Suspension fixture check status%u f%u b%u i%u reason%u\n",
            checked_status,diagnostic.function,diagnostic.block,diagnostic.instruction,diagnostic.reason);
    CHECK(checked_status==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);xr_xir_compile_artifact_free(closed);
    return lowered;
}

static void suspended_result42(XrXirProgram *program) {
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,1,NULL,0)==XR_XIR_CALL_READY);
    for(unsigned suspension=0;suspension<2;++suspension) {
        XrXirInstanceResult step=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
        CHECK(step.outcome.status==XR_XIR_CALL_SUSPENDED && step.outcome.wake);
        CHECK(xr_xir_instance_resume(instance,step.epoch,step.outcome.wake)==XR_XIR_CALL_READY);
    }
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==XR_XIR_I64 && result.payload==42);xr_xir_value_drop(&result);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
}

int main(int argc,char **argv) {
    CHECK(argc==2);unsigned variant=(unsigned)strtoul(argv[1],NULL,10);CHECK(variant<4);
    const XrCompileResourceLimits limits={67108864,8388608,128000000};
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *artifact=suspension_program(&context,variant);
    XrXirEffects *effects=artifact->program_final;
    EffectFinalOutcomes *final=effects->invocations->final->outcomes;
    CHECK(xir_fixture_effects_program_root(&context,
        (XirFixtureEffectInput){&artifact->module,artifact->construction},effects)==XR_XIR_OK);
    XrXirEffectWitness saved[2]={final->witnesses[1],final->witnesses[2]};
    if(variant<3) {
        uint32_t instruction=variant?1:0,target=variant==2?3:2,distance=variant==2?2:1;
        CHECK(effect_control_cause_is(&saved[0],XR_XIR_EFFECT_CAUSE_CALL,instruction,target,distance));
        final->witnesses[1]=(XrXirEffectWitness){XR_XIR_EFFECT_CAUSE_CALL,variant?0:1,
            variant==1?2:variant==2?2:3,distance};
    } else for(uint32_t f=1;f<3;++f) {
        CHECK(final->functions[f].suspend==XR_XIR_EFFECT_NONE);
        final->functions[f].suspend=XR_XIR_EFFECT_MAY;
        final->witnesses[f]=(XrXirEffectWitness){XR_XIR_EFFECT_CAUSE_CALL,0,3-f,1};
    }
    CHECK(xir_fixture_effects_program_root(&context,
        (XirFixtureEffectInput){&artifact->module,artifact->construction},effects)==XR_XIR_BAD_STRUCTURE);
    final->witnesses[1]=saved[0];final->witnesses[2]=saved[1];
    if(variant==3) for(uint32_t f=1;f<3;++f) final->functions[f].suspend=XR_XIR_EFFECT_NONE;
    CHECK(xir_fixture_effects_program_root(&context,
        (XirFixtureEffectInput){&artifact->module,artifact->construction},effects)==XR_XIR_OK);
    if(variant<3) {
        XrXirProgram *program=NULL;
        CHECK(xr_xir_compile_vm_program_take(&artifact,&program)==XR_XIR_OK && !artifact);
        suspended_result42(program);xr_xir_compile_program_drop(program);
    } else xr_xir_compile_artifact_free(artifact);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==baseline.live_bytes);xr_compile_resources_release(context.resources);
    return 0;
}
