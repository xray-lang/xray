/* Genuine Final payloads reject unreadable current views before semantic use. */
#define main storage_fixture_entry
#include "test_xir_program_input_storage.c"
#undef main

int main(int argc,char **argv) {
    CHECK(argc==2);
    unsigned attack=(unsigned)strtoul(argv[1],NULL,10);
    CHECK(attack<33);
    const XrCompileResourceLimits limits={67108864,8388608,128000000};
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *artifact=program_input(&context);
    XrXirEffects *effects=artifact->program_final;
    CHECK(xir_effects_program_final_verify(&context,&artifact->module,effects)==XR_XIR_OK);
    EffectInvocationCertificate *certificate=effects->invocations;
    EffectInvocationFinal *graph=certificate->final;
    CHECK(graph->count>=3 && graph->nodes[2].parameter_count==1);
    EffectFinalOutcomes *outcomes=graph->outcomes;
    EffectInvocationCertificate saved_certificate=*certificate;
    EffectInvocationFinal saved_graph=*graph;
    EffectFinalOutcomes saved_outcomes=*outcomes;
    EffectInvocationNode saved_node=graph->nodes[2];
    EffectPhysicalFunction saved_physical=graph->physical->functions[2];
    EffectInvocationFunctionBounds saved_bounds=certificate->declared->functions[2];
    XrXirFunctionEffectContract saved_contract=outcomes->contracts[2];
    EffectFinalFunction saved_summary=graph->summary.functions[2];
    XrXirOrigin saved_origin=certificate->origins[2];
    uint8_t *saved_kind=certificate->parameter_kinds[2];
    uint32_t *saved_entry=(uint32_t *)outcomes->entry_roots[2].parameters;
    uint32_t saved_entry_count=outcomes->entry_roots[2].parameter_count;
    if(attack==0) certificate->final=(EffectInvocationFinal *)(uintptr_t)1;
    if(attack==1) graph->nodes=(EffectInvocationNode *)(uintptr_t)1;
    if(attack==2) { graph->edges=(EffectInvocationEdge *)(uintptr_t)1;graph->edge_capacity=1; }
    if(attack==3) { graph->sites=(EffectInvocationSite *)(uintptr_t)1;graph->site_count=1; }
    if(attack==4) { graph->deferred_calls=(EffectInvocationDeferred *)(uintptr_t)1;graph->deferred_capacity=1; }
    if(attack==5) graph->roots=(uint32_t *)(uintptr_t)1;
    if(attack==6) graph->root_spaces=(uint32_t *)(uintptr_t)1;
    if(attack==7) graph->nodes[2].causes=(EffectInvocationCause *)(uintptr_t)1;
    if(attack==8) graph->nodes[2].input=(uint64_t *)(uintptr_t)1;
    if(attack==9) graph->nodes[2].cells=(uint64_t *)(uintptr_t)1;
    if(attack==10) graph->nodes[2].local_cells=(uint64_t *)(uintptr_t)1;
    if(attack==11) graph->nodes[2].parameter_coverage=(uint64_t *)(uintptr_t)1;
    if(attack==12) { graph->nodes[2].producers=(EffectInvocationProducer *)(uintptr_t)1;graph->nodes[2].producer_capacity=1; }
    if(attack==13) { graph->nodes[2].classes=(EffectInvocationClassFact *)(uintptr_t)1;graph->nodes[2].class_count=1; }
    if(attack==14) certificate->declared=(EffectInvocationDeclaredBounds *)(uintptr_t)1;
    if(attack==15) { certificate->declared->functions[2].bounds=(EffectInvocationValueBound *)(uintptr_t)1;certificate->declared->functions[2].count=1; }
    if(attack==16) graph->physical=(EffectPhysicalDomain *)(uintptr_t)1;
    if(attack==17) { graph->physical->functions[2].records=(EffectPhysicalRecord *)(uintptr_t)1;graph->physical->functions[2].capacity=1; }
    if(attack==18) effects->outcomes_owner=(EffectFinalOutcomes *)(uintptr_t)1;
    if(attack==19) outcomes->root=(XrXirRootEffects *)(uintptr_t)1;
    if(attack==20) outcomes->functions=(XrXirFunctionEffects *)(uintptr_t)1;
    if(attack==21) outcomes->contracts=(XrXirFunctionEffectContract *)(uintptr_t)1;
    if(attack==22) outcomes->contracts[2].parameters=(XrXirEffectParameter *)(uintptr_t)1;
    if(attack==23) { outcomes->errors=(uint64_t *)(uintptr_t)1;outcomes->words=1;outcomes->error_rows=1; }
    if(attack==24) { outcomes->entry_roots[2].parameters=(uint32_t *)(uintptr_t)1;outcomes->entry_roots[2].parameter_count=1; }
    if(attack==25) graph->summary.functions=(EffectFinalFunction *)(uintptr_t)1;
    if(attack==26) graph->summary.witnesses=(XrXirRootEffectWitness *)(uintptr_t)1;
    if(attack==27) { graph->summary.functions[2].storage=(XrXirRootTerm *)(uintptr_t)1;graph->summary.functions[2].capacity=1; }
    if(attack==28) certificate->origins=(XrXirOrigin *)(uintptr_t)1;
    if(attack==29) { certificate->origins[2].arguments=(XrXirType *)(uintptr_t)1;certificate->origins[2].argument_count=1; }
    if(attack==30) certificate->parameter_kinds[2]=(uint8_t *)(uintptr_t)1;
    if(attack==31) graph->count=graph->capacity+1;
    if(attack==32) graph->nodes=(EffectInvocationNode *)((uint8_t *)graph->nodes+1);
    XrXirStatus status=xir_effects_program_final_verify(&context,&artifact->module,effects);
    fprintf(stderr,"Final storage attack%u status%u\n",attack,status);
    *certificate=saved_certificate;*graph=saved_graph;*outcomes=saved_outcomes;
    graph->nodes[2]=saved_node;
    graph->physical->functions[2]=saved_physical;
    certificate->declared->functions[2]=saved_bounds;
    outcomes->contracts[2]=saved_contract;
    graph->summary.functions[2]=saved_summary;
    certificate->origins[2]=saved_origin;certificate->parameter_kinds[2]=saved_kind;
    outcomes->entry_roots[2].parameters=saved_entry;
    outcomes->entry_roots[2].parameter_count=saved_entry_count;
    effects->outcomes_owner=NULL;
    CHECK(status==XR_XIR_BAD_STRUCTURE);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&artifact,&program)==XR_XIR_OK && !artifact);
    result42(program);xr_xir_compile_program_drop(program);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==baseline.live_bytes);
    xr_compile_resources_release(context.resources);
    return 0;
}
