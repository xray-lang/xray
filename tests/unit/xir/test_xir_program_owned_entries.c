/* A failed owning receive must retain the caller's physical entry table. */
#define main owned_entries_callback_fixture_entry
#include "test_xir_program_owned_callbacks.c"
#undef main

static XrXirArtifact *entries_artifact(const XrXirCompileContext *context) {
    const XrXirInstruction init={.op=XR_XIR_RETURN};
    const XrXirInstruction main_ops[]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42},
        {.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,1},.immediate=2},
        {.op=XR_XIR_RETURN,.args={1}}};
    const XrXirInstruction identity={.op=XR_XIR_RETURN,.args={0}};
    const XrXirType parameter=XR_XIR_I64;const uint32_t operand=0;
    const XrXirBlock blocks[]={{0,1,0,0},{0,3,0,0},{0,1,0,0}};
    const XrXirFunction functions[]={
        {"init",4,NULL,0,XR_XIR_UNIT,blocks,1,&init,1,NULL,0},
        {"main",4,NULL,0,XR_XIR_I64,blocks+1,1,main_ops,3,&operand,1},
        {"identity",8,&parameter,1,XR_XIR_I64,blocks+2,1,&identity,1,NULL,0}};
    const XrXirSourceModule source={"root",4,NULL,0,0};
    const XrXirFunctionIdentity identities[]={
        {0,0,0,0,0,0,XR_XIR_NON_MEMBER,0,0},
        {0,0,0,0,0,0,XR_XIR_NON_MEMBER,0,0},
        {0,0,0,0,0,0,XR_XIR_NON_MEMBER,0,0}};
    const XrXirDeclarations declarations={&source,1,identities,NULL,0,NULL,0,0,1,NULL};
    const XrXirModule module={XR_XIR_BUILT,functions,3,&declarations,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xir_fixture_check(context,&module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);xr_xir_compile_artifact_free(closed);return lowered;
}
static void receive_parts(const XrXirCompileContext *context,XrXirProgramInput **input,
    XrXirCallEntry **entries,XrXirVmBinding bindings[3],XrXirProgramSpec *spec) {
    XrXirArtifact *artifact=entries_artifact(context);XrXirStatus status=XR_XIR_OK;
    *entries=xir_compile_calloc(context,3,sizeof(**entries),&status);CHECK(status==XR_XIR_OK);
    for(uint32_t f=0;f<3;++f) {
        CHECK(xr_xir_compile_vm_bind(artifact,f,&bindings[f],&(*entries)[f])==XR_XIR_OK);
        const XrXirType *parameters=(*entries)[f].parameters;
        (*entries)[f].parameters=xir_compile_copy(context,parameters,
            (size_t)(*entries)[f].parameter_count*sizeof(*parameters),&status);
        CHECK(status==XR_XIR_OK);
    }
    *spec=(XrXirProgramSpec){XR_XIR_PROGRAM_ABI_VERSION,artifact->target,*entries,3,
        artifact->module.declarations,{0},artifact->module.types,{0}};
    CHECK(xir_program_input_take(context,&artifact,input)==XR_XIR_OK && !artifact);
}

int main(int argc,char **argv) {
    CHECK(argc==2 && (!strcmp(argv[1],"free") || !strcmp(argv[1],"alloc")));
    const XrCompileResourceLimits caps={67108864,8388608,128000000};
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirProgramInput *input=NULL;XrXirCallEntry *entries=NULL;XrXirVmBinding bindings[3];XrXirProgramSpec spec={0};XrXirProgram *program=NULL;
    receive_parts(&context,&input,&entries,bindings,&spec);
    probe_artifact=input->artifact;probe_attack=15;probe_allocations=!strcmp(argv[1],"alloc");probe_at=SIZE_MAX;probe_calls=0;probe_injected=false;probe_armed=true;
    CHECK(xir_program_receive(&context,&spec,&input,&entries,&program)==XR_XIR_OK && !input && !entries);
    probe_armed=false;size_t frees=probe_calls;CHECK(frees && !probe_injected && execute(program)==42);xr_xir_compile_program_drop(program);program=NULL;
    receive_parts(&context,&input,&entries,bindings,&spec);
    XrXirProgramInput *original_input=input;XrXirCallEntry *original_entries=entries;
    XrXirCallEntry snapshot[3];memcpy(snapshot,entries,sizeof(snapshot));
    const XrXirType *original_parameters=entries[2].parameters;XrXirType parameter=snapshot[2].parameters[0];CHECK(parameter==XR_XIR_I64);
    probe_artifact=input->artifact;probe_at=frees-1;probe_calls=0;probe_injected=false;probe_armed=true;
    XrXirStatus status=xir_program_receive(&context,&spec,&input,&entries,&program);probe_armed=false;
    CHECK(probe_injected && status==XR_XIR_BAD_STRUCTURE && !program && input==original_input);
    bool readable=xr_compile_resources_extent(context.resources,original_entries,sizeof(snapshot))==XR_COMPILE_RESOURCE_OK;
    bool parameter_live=xr_compile_resources_extent(context.resources,original_parameters,sizeof(parameter))==XR_COMPILE_RESOURCE_OK;
    bool preserved=entries==original_entries && readable && parameter_live;
    printf("owned entry last-%s=%zu status=%u input_preserved=1 entry_slot_preserved=%u original_table_live=%u parameter_live=%u\n",argv[1],frees-1,status,entries==original_entries,readable,parameter_live);
    if(preserved)CHECK(!memcmp(snapshot,entries,sizeof(snapshot)));
    final_restore();
    if(!preserved){XrXirStatus allocated=XR_XIR_OK;entries=xir_compile_copy(&context,snapshot,sizeof(snapshot),&allocated);CHECK(allocated==XR_XIR_OK);entries[2].parameters=xir_compile_copy(&context,&parameter,sizeof(parameter),&allocated);CHECK(allocated==XR_XIR_OK);spec.entries=entries;}
    CHECK(xir_program_receive(&context,&spec,&input,&entries,&program)==XR_XIR_OK && !input && !entries);
    CHECK(execute(program)==42);xr_xir_compile_program_drop(program);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==before.live_bytes);
    xr_compile_resources_release(context.resources);
    printf("original entry ownership preserved=%u; input same-owner recovery42; physical baseline recovered; expected1\n",preserved);
    return preserved?0:1;
}
