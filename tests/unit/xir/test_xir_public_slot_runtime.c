/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_public_slot_runtime.c - Actual slot facts at live callback boundaries
 *
 * KEY CONCEPT:
 *   Trusted entries attempt unauthorized reads against real sealed Programs.
 *   A receiving descriptor cannot promote private birth storage to public.
 */
#define main public_slot_source_regressions
#include "test_xir_exported_i64_const.c"
#undef main
#include "xir/xxir_instance.c"

static uint32_t public_slot, private_slot, mutable_slot, public_entry, private_entry;
static unsigned runtime_mode, runtime_observations;
static XrXirAction (*runtime_resume)(XrXirCallView *);
static XrXirCallStatus runtime_expected;
static XrXirAction runtime_probe(XrXirCallView *view) {
    XrXirInstance *instance=view_instance(view);
    CHECK(instance && instance->state==XR_XIR_INSTANCE_READY);
    XrXirDeclarations *d=(XrXirDeclarations *)instance->program->declarations;
    uint32_t slot=runtime_mode==1?private_slot:runtime_mode==2?mutable_slot:public_slot;
    uint32_t from=d->functions[xr_xir_call_current_entry(view->activation)].module;
    XrXirSourceModule *modules=(XrXirSourceModule *)d->modules;
    XrXirSourceModule saved=modules[from];uint32_t dependencies[64],count=0;
    uint8_t published=instance->published[slot];
    InstanceStorageRoute *saved_routes=instance->slot_origins,routes[64]={{0}};
    CHECK(d->slot_count<=64);
    if(runtime_mode==3)instance->published[slot]=0;
    if(runtime_mode==4) {
        CHECK(saved.dependency_count<=64);
        for(uint32_t i=0;i<saved.dependency_count;++i)
            if(saved.dependencies[i]!=d->slots[slot].module)dependencies[count++]=saved.dependencies[i];
        CHECK(count+1==saved.dependency_count && count);
        modules[from].dependencies=dependencies;modules[from].dependency_count=count;
    }
    if(runtime_mode==5 || runtime_mode==6) {
        XrXirInstance *birth=instance->previous_generation;
        CHECK(birth && birth->domain==instance->domain && birth->program!=instance->program);
        CHECK(birth->program->declarations->slot_count==d->slot_count);
        uint32_t origin=runtime_mode==5?private_slot:public_slot;
        const XrXirSlotFacts *facts=instance_slot_facts(birth);
        CHECK(facts && facts->rows[origin].exported==(runtime_mode==5?0u:1u));
        CHECK(birth->program->declarations->slots[origin].type==d->slots[slot].type);
        routes[slot]=(InstanceStorageRoute){birth,origin};instance->slot_origins=routes;
    }
    XrXirValue value={0};XrXirCallStatus status;
    if(runtime_mode==7) {
        XrXirValue input={XR_XIR_I64,0,99};
        status=xr_xir_instance_slot_write(view,slot,&input,true);
    } else if(runtime_mode==8) {
        XrXirValueReceiver receiver={.kind=XR_XIR_ROOT_SLOT,.type=XR_XIR_I64,.slot=slot};
        XrXirValuePlace place={0};
        status=value_place(view,instance,&receiver,false,xr_xir_call_admission(view),&place);
        CHECK(!place.payload);
    } else status=xr_xir_instance_slot_read(view,slot,&value);
    instance->published[slot]=published;modules[from]=saved;instance->slot_origins=saved_routes;
    if(status!=runtime_expected)fprintf(stderr,"slot mode%u status%u expected%u\n",runtime_mode,status,runtime_expected);
    CHECK(status==runtime_expected);
    if(status==XR_XIR_CALL_READY)CHECK(value.type==XR_XIR_I64 && value.payload==(runtime_mode==1?0:1));
    else CHECK(!value.type && !value.reserved && !value.payload);
    xr_xir_value_drop(&value);++runtime_observations;
    return runtime_resume(view);
}
static void runtime_case(XrXirInstance *owner,unsigned mode,bool same_scope) {
    XrXirInstance *state=instance_current(owner);uint32_t entry=same_scope?private_entry:public_entry;
    runtime_mode=mode;runtime_expected=mode==0 || mode==6 || same_scope?XR_XIR_CALL_READY:XR_XIR_CALL_BAD_STATE;
    runtime_resume=state->entries[entry].resume;state->entries[entry].resume=runtime_probe;
    unsigned before=runtime_observations;XrXirValue answer=ci_take(owner,entry);
    state->entries[entry].resume=runtime_resume;
    ci_integer(&answer,same_scope?1:7);CHECK(runtime_observations>before);
}
static XrXirProgram *runtime_program(const XrXirCompileContext *context,const XrXirArtifact *closed) {
    XrXirArtifact *lowered=NULL;XrXirProgram *program=NULL;
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    (void)context;return program;
}
int main(void) {
    ci_work(false);XrXirCompileContext context=ci_context();
    ci_file("other.xr",ci_other_source);ci_file("flags.xr",ci_flags_source);
    ci_file("facade.xr",ci_facade_source);ci_file("hop.xr",ci_hop_source);
    XrXirSourceResult source=ci_source(&context,ci_consumer_source,"entry.xr",XR_XIR_PROGRAM,NULL);
    XrXirArtifact *closed=NULL;CHECK(xr_xir_compile_specialize(source.checked,&closed,NULL,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    const XrXirModule *module=xr_xir_compile_artifact_module(closed);
    public_entry=ci_entry(module,"all");private_entry=UINT32_MAX;
    uint32_t flags=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==5 && !memcmp(module->functions[f].name,"plain",5)) {
            CHECK(private_entry==UINT32_MAX);private_entry=f;flags=module->declarations->functions[f].module;
        }
    CHECK(private_entry!=UINT32_MAX);
    public_slot=private_slot=mutable_slot=UINT32_MAX;
    const XrXirSlotFacts *facts=xr_xir_compile_construction_slots(xr_xir_compile_artifact_construction(closed));
    for(uint32_t s=0;s<facts->count;++s)if(module->declarations->slots[s].module==flags) {
        const XrXirSlotFact *fact=&facts->rows[s];
        if(fact->name_length==9 && !memcmp(fact->name,"PROT_READ",9))public_slot=s;
        if(fact->name_length==11 && !memcmp(fact->name,"initialized",11))private_slot=s;
        if(fact->name_length==10 && !memcmp(fact->name,"initMarker",10))mutable_slot=s;
    }
    CHECK(public_slot!=UINT32_MAX && private_slot!=UINT32_MAX && mutable_slot!=UINT32_MAX);
    XrXirProgram *first=runtime_program(&context,closed),*second=runtime_program(&context,closed);
    xr_xir_compile_artifact_free(closed);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_generation_new(&config,&instance)==XR_XIR_CALL_READY);
    uint64_t generation=0;CHECK(xr_xir_instance_generation_prepare(instance,first,&generation)==XR_XIR_CALL_READY);
    XrXirValue answer=ci_take(instance,public_entry);ci_integer(&answer,7);
    CHECK(xr_xir_instance_generation_publish(instance,generation)==XR_XIR_CALL_READY);
    for(unsigned mode=0;mode<5;++mode)runtime_case(instance,mode,false);
    runtime_case(instance,1,true);runtime_case(instance,7,false);runtime_case(instance,8,false);
    generation=0;CHECK(xr_xir_instance_generation_prepare(instance,second,&generation)==XR_XIR_CALL_READY);
    answer=ci_take(instance,public_entry);ci_integer(&answer,7);
    CHECK(xr_xir_instance_generation_publish(instance,generation)==XR_XIR_CALL_READY);
    runtime_case(instance,5,false);runtime_case(instance,6,false);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(first);xr_xir_compile_program_drop(second);
    xr_compile_resources_release(context.resources);
    puts("actual public/private/mutable/direct-import/publication/birth and same-scope slot routes PASS");
    return 0;
}
