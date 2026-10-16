/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_instance_bindings.c - Exact physical binding publication and shadowing
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xir/xxir_instance_binding.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#define XIR_INSTANCE_IMPLEMENTATION_PRESENT
#include "xir_instance_retirement_probe.c"
#undef XIR_INSTANCE_IMPLEMENTATION_PRESENT
#undef CHECK
#define main original_instance_regressions
#include "test_xir_instances.c"
#undef main
#include "xir/xxir_vm.h"

typedef struct BindingEntry {
    XrXirVmBinding vm;
    XrXirAction (*resume)(XrXirCallView *);
    const XrXirInstanceBinding *observe;
    XrXirCallStatus observed;
    uint32_t observations, resumes;
} BindingEntry;
typedef struct BindingImage {
    XrXirArtifact *artifact;
    BindingEntry entries[4];
    unsigned *releases;
} BindingImage;
static XrXirAction binding_resume(XrXirCallView *view) {
    BindingEntry *entry=(BindingEntry *)view->environment;
    ++entry->resumes;
    if (entry->observe) {
        XrXirValue value={0};
        entry->observed=xr_xir_instance_binding_slot_read(view,entry->observe,&value);
        ++entry->observations;
        xr_xir_value_drop(&value);
    }
    return entry->resume(view);
}
static void binding_image_free(void *opaque) {
    BindingImage *image=opaque;
    ++*image->releases;
    xr_xir_compile_artifact_free(image->artifact);
    xr_free(image);
}
static void binding_program(bool boolean,int64_t answer,bool exported,unsigned *releases,
    XrXirProgram **output,BindingImage **borrow) {
    XrXirType element=boolean?XR_XIR_BOOL:XR_XIR_I64;
    XrXirTypeNode node={.kind=XR_XIR_TYPE_CELL,.element=element};
    XrXirTypes types={.nodes=&node,.count=1};
    XrXirInstruction initialize[]={
        {.op=boolean?XR_XIR_CONST_BOOL:XR_XIR_CONST_INT,.type=element,.immediate=boolean?1:11},
        {.op=XR_XIR_CELL_NEW,.type=(XrXirType)256,.args={0,0}},
        {.op=XR_XIR_SLOT_INIT,.args={1,0}}, {.op=XR_XIR_RETURN}};
    XrXirInstruction entry[]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},{.op=XR_XIR_RETURN,.args={0,0}}};
    XrXirInstruction function[]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=answer},{.op=XR_XIR_RETURN,.args={0,0}}};
    XrXirInstruction reader[]={{.op=XR_XIR_SLOT_LOAD,.type=(XrXirType)256},
        {.op=XR_XIR_CELL_READ,.type=element,.args={0,0}},{.op=XR_XIR_RETURN,.args={1,0}}};
    XrXirBlock blocks[]={{.count=4},{.count=2},{.count=2},{.count=3}};
    XrXirFunction functions[]={
        {.name="init",.name_length=4,.result=XR_XIR_UNIT,.blocks=&blocks[0],.block_count=1,.instructions=initialize,.instruction_count=4},
        {.name="entry",.name_length=5,.result=XR_XIR_I64,.blocks=&blocks[1],.block_count=1,.instructions=entry,.instruction_count=2},
        {.name="answer",.name_length=6,.result=XR_XIR_I64,.blocks=&blocks[2],.block_count=1,.instructions=function,.instruction_count=2},
        {.name="read",.name_length=4,.result=element,.blocks=&blocks[3],.block_count=1,.instructions=reader,.instruction_count=3}};
    XrXirFunctionIdentity identities[4]={0};
    identities[2].exported=exported;identities[3].exported=exported;
    XrXirSourceModule source={"root",4,NULL,0,0,0,XR_XIR_MODULE_FRESH_STORAGE};
    XrXirSlot slot={0,(XrXirType)256,1};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,
        .slots=&slot,.slot_count=1,.entry_function=1};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=functions,.function_count=4,
        .declarations=&declarations,.types=&types,.linkage_kind=XR_XIR_PROGRAM};
    NativeFixtureOwner compiler={0};CHECK(native_fixture_owner_new(&compiler)==XR_XIR_OK);
    XrXirArtifact *checked=NULL;CHECK(xir_fixture_check(&compiler.context,&module,&checked,NULL)==XR_XIR_OK);
    BindingImage *image=xr_malloc(sizeof(*image));CHECK(image);memset(image,0,sizeof(*image));image->releases=releases;
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(checked,&target,&image->artifact,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    const XrXirModule *lowered=xr_xir_compile_artifact_module(image->artifact);
    XrXirCallEntry entries[4];
    for (uint32_t i=0;i<4;++i) {
        CHECK(xr_xir_compile_vm_bind(image->artifact,i,&image->entries[i].vm,&entries[i])==XR_XIR_OK);
        image->entries[i].resume=entries[i].resume;
        entries[i].resume=binding_resume;entries[i].environment=&image->entries[i];
    }
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,target,entries,4,lowered->declarations,
        {image,binding_image_free},lowered->types,xr_xir_compile_program_proof(image->artifact)};
    CHECK(xr_xir_compile_program_seal(&compiler.context,&spec,output)==XR_XIR_OK);
    native_fixture_owner_free(&compiler);*borrow=image;
}
static XrXirInstanceResult binding_drive(XrXirInstance *instance) {
    XrXirInstanceResult result;
    do {result=xr_xir_instance_poll_bounded(instance,256);} while(result.outcome.status==XR_XIR_CALL_READY);
    return result;
}
static const XrXirInstanceBinding *binding_lookup(XrXirInstance *instance,const char *symbol,uint32_t length) {
    const XrXirInstanceBinding *binding=NULL;
    CHECK(xr_xir_instance_binding_lookup(instance,symbol,length,&binding)==XR_XIR_CALL_READY && binding);
    return binding;
}
static void binding_expect(XrXirInstance *instance,const XrXirInstanceBinding *binding,XrXirType type,int64_t value) {
    XrXirCallStatus started=xr_xir_instance_binding_start(instance,binding,NULL,0);
    if (started!=XR_XIR_CALL_READY) fprintf(stderr,"binding start generation%llu ordinal%u kind%u status%u state%u prepared%u\n",
        (unsigned long long)binding->birth->generation_id,binding->ordinal,binding->kind,started,
        binding->birth->state,instance->prepared_generation!=NULL);
    CHECK(started==XR_XIR_CALL_READY);
    CHECK(binding_drive(instance).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==(uint32_t)type && result.payload==value);xr_xir_value_drop(&result);
}
static void bindings_normal(void) {
    size_t blocks=runtime_live,bytes=runtime_bytes;
    unsigned old_releases=0,new_releases=0,failed_releases=0;
    BindingImage *old_image=NULL,*new_image=NULL,*failed_image=NULL;
    XrXirProgram *old=NULL,*newer=NULL,*failed=NULL;
    binding_program(false,42,true,&old_releases,&old,&old_image);
    binding_program(true,99,true,&new_releases,&newer,&new_image);
    binding_program(false,77,false,&failed_releases,&failed,&failed_image);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_generation_new(&config,&instance)==XR_XIR_CALL_READY);
    uint64_t serial=0;CHECK(xr_xir_instance_generation_prepare(instance,old,&serial)==XR_XIR_CALL_READY);
    char key[]={'x'};
    XrXirInstanceBindingSpec specs[]={{key,1,XR_XIR_INSTANCE_BINDING_SLOT,0},
        {"f",1,XR_XIR_INSTANCE_BINDING_FUNCTION,2},{"reader",6,XR_XIR_INSTANCE_BINDING_FUNCTION,3}};
    XrXirInstanceBindingSpec duplicate[]={{"d",1,XR_XIR_INSTANCE_BINDING_SLOT,0},{"d",1,XR_XIR_INSTANCE_BINDING_FUNCTION,2}};
    CHECK(xr_xir_instance_generation_bind(instance,serial,duplicate,2)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!instance->prepared_generation->symbol_bindings);
    XrXirInstanceBindingSpec invalid={"bad",3,XR_XIR_INSTANCE_BINDING_FUNCTION,0};
    CHECK(xr_xir_instance_generation_bind(instance,serial,&invalid,1)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_generation_bind(instance,serial,specs,3)==XR_XIR_CALL_READY);
    key[0]='z';
    CHECK(xr_xir_instance_generation_bind(instance,serial,specs,3)==XR_XIR_CALL_BAD_STATE);
    const XrXirInstanceBinding *absent=NULL;
    CHECK(xr_xir_instance_binding_lookup(instance,"x",1,&absent)==XR_XIR_CALL_BAD_STATE && !absent);
    CHECK(xr_xir_instance_start(instance,1,NULL,0)==XR_XIR_CALL_READY);
    CHECK(binding_drive(instance).outcome.status==XR_XIR_CALL_RETURNED);
    size_t attempts=runtime_attempts;
    uint64_t work=xr_xir_domain_budget_stats(instance->domain).work;
    CHECK(xr_xir_instance_generation_publish(instance,serial)==XR_XIR_CALL_READY);
    CHECK(runtime_attempts==attempts && xr_xir_domain_budget_stats(instance->domain).work==work);
    uint32_t old_initializations=old_image->entries[0].resumes;
    CHECK(old_initializations);
    const XrXirInstanceBinding *old_slot=binding_lookup(instance,"x",1),*old_function=binding_lookup(instance,"f",1);
    const XrXirInstanceBinding *old_reader=binding_lookup(instance,"reader",6);
    old_image->entries[3].observe=old_slot;
    XrXirInstanceBindingView old_view={0};CHECK(xr_xir_instance_binding_view(instance,old_slot,&old_view)==XR_XIR_CALL_READY);
    CHECK(old_view.generation==serial && old_view.program==old && old_view.slot_type==(XrXirType)256 && old_view.mutable);
    CHECK(xr_xir_cell_element(old_view.types,old_view.slot_type)==XR_XIR_I64);
    binding_expect(instance,old_function,XR_XIR_I64,42);
    binding_expect(instance,old_reader,XR_XIR_I64,11);
    CHECK(old_image->entries[3].observations && old_image->entries[3].observed==XR_XIR_CALL_READY);
    uint64_t second=0;CHECK(xr_xir_instance_generation_prepare(instance,newer,&second)==XR_XIR_CALL_READY);
    key[0]='x';CHECK(xr_xir_instance_generation_bind(instance,second,specs,3)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_binding_start(instance,old_function,NULL,0)==XR_XIR_CALL_BUSY);
    CHECK(binding_lookup(instance,"x",1)==old_slot);
    CHECK(xr_xir_instance_start(instance,1,NULL,0)==XR_XIR_CALL_READY);
    CHECK(binding_drive(instance).outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_generation_publish(instance,second)==XR_XIR_CALL_READY);
    uint32_t new_initializations=new_image->entries[0].resumes;
    CHECK(new_initializations);
    const XrXirInstanceBinding *new_slot=binding_lookup(instance,"x",1),*new_reader=binding_lookup(instance,"reader",6);
    CHECK(new_slot!=old_slot);
    XrXirInstanceBindingView new_view={0};CHECK(xr_xir_instance_binding_view(instance,new_slot,&new_view)==XR_XIR_CALL_READY);
    CHECK(new_view.program==newer && new_view.arena!=old_view.arena && new_view.slot_type==old_view.slot_type);
    CHECK(xr_xir_cell_element(new_view.types,new_view.slot_type)==XR_XIR_BOOL);
    new_image->entries[3].observe=old_slot;
    binding_expect(instance,new_reader,XR_XIR_BOOL,1);
    CHECK(new_image->entries[3].observed==XR_XIR_CALL_BAD_STATE);
    new_image->entries[3].observe=new_slot;
    binding_expect(instance,new_reader,XR_XIR_BOOL,1);
    CHECK(new_image->entries[3].observed==XR_XIR_CALL_READY);
    binding_expect(instance,binding_lookup(instance,"f",1),XR_XIR_I64,99);
    binding_expect(instance,old_function,XR_XIR_I64,42);
    binding_expect(instance,old_reader,XR_XIR_I64,11);
    XrXirInstanceBinding forged=*old_slot;XrXirInstanceBindingView sentinel={0};
    CHECK(xr_xir_instance_binding_view(instance,&forged,&sentinel)==XR_XIR_CALL_BAD_ARGUMENT && !sentinel.program);
    XrXirInstance *other=NULL;CHECK(xr_xir_instance_generation_new(&config,&other)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_binding_view(other,old_slot,&sentinel)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_free(other)==XR_XIR_CALL_READY);
    uint64_t third=0;CHECK(xr_xir_instance_generation_prepare(instance,failed,&third)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_generation_bind(instance,third,specs,3)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,1,NULL,0)==XR_XIR_CALL_READY);
    CHECK(binding_drive(instance).outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_generation_abort(instance,third)==XR_XIR_CALL_READY);
    CHECK(binding_lookup(instance,"x",1)==new_slot);
    binding_expect(instance,old_function,XR_XIR_I64,42);
    binding_expect(instance,old_reader,XR_XIR_I64,11);
    uint64_t private_serial=0;
    CHECK(xr_xir_instance_generation_prepare(instance,failed,&private_serial)==XR_XIR_CALL_READY);
    CHECK(private_serial>third);
    CHECK(xr_xir_instance_generation_bind(instance,private_serial,specs,3)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,1,NULL,0)==XR_XIR_CALL_READY);
    CHECK(binding_drive(instance).outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_generation_publish(instance,private_serial)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_binding_start(instance,binding_lookup(instance,"f",1),NULL,0)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(old_image->entries[0].resumes==old_initializations && new_image->entries[0].resumes==new_initializations);
    xr_xir_compile_program_drop(old);xr_xir_compile_program_drop(newer);xr_xir_compile_program_drop(failed);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(old_releases==1 && new_releases==1 && failed_releases==1);
    CHECK(runtime_live==blocks && runtime_bytes==bytes);
    puts("binding old42/new99; old Int11/new Bool1; copied keys; abort preserves published map; physical baseline restored");
}
static XrXirCallStatus binding_prefix(XrXirProgram *program,const XrXirInstanceConfig *config,
    XrXirInstance **instance,uint64_t *serial) {
    XrXirCallStatus status=xr_xir_instance_generation_new(config,instance);
    if (status==XR_XIR_CALL_READY) status=xr_xir_instance_generation_prepare(*instance,program,serial);
    return status;
}
static void binding_prefix_free(XrXirInstance *instance,uint64_t serial) {
    if (!instance) return;
    if (serial) CHECK(xr_xir_instance_generation_abort(instance,serial)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
}
static void bindings_prepare_resources(void) {
    size_t physical=runtime_live,physical_bytes=runtime_bytes;
    unsigned releases=0;BindingImage *image=NULL;XrXirProgram *program=NULL;
    binding_program(false,42,true,&releases,&program,&image);
    XrXirInstanceBindingSpec specs[]={{"x",1,XR_XIR_INSTANCE_BINDING_SLOT,0},
        {"f",1,XR_XIR_INSTANCE_BINDING_FUNCTION,2},{"reader",6,XR_XIR_INSTANCE_BINDING_FUNCTION,3}};
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;uint64_t serial=0;
    CHECK(binding_prefix(program,&config,&instance,&serial)==XR_XIR_CALL_READY);
    size_t start=runtime_attempts;
    CHECK(xr_xir_instance_generation_bind(instance,serial,specs,3)==XR_XIR_CALL_READY);
    size_t sites=runtime_attempts-start;CHECK(sites);
    XrXirDomainBudgetStats required=xr_xir_domain_budget_stats(instance->domain);
    binding_prefix_free(instance,serial);
    size_t baseline=runtime_live,baseline_bytes=runtime_bytes;
    for (size_t site=0;site<sites;++site) {
        instance=NULL;serial=0;CHECK(binding_prefix(program,&config,&instance,&serial)==XR_XIR_CALL_READY);
        XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(instance->domain);
        runtime_fail_at=runtime_attempts+site;
        CHECK(xr_xir_instance_generation_bind(instance,serial,specs,3)==XR_XIR_CALL_OOM);
        CHECK(runtime_attempts>runtime_fail_at && !instance->prepared_generation->symbol_bindings);
        runtime_fail_at=SIZE_MAX;
        XrXirDomainBudgetStats failed=xr_xir_domain_budget_stats(instance->domain);
        CHECK(failed.metadata_live==before.metadata_live && failed.requested_call_bytes>before.requested_call_bytes &&
            failed.requested_bytes==before.requested_bytes && failed.work>before.work);
        CHECK(xr_xir_instance_generation_bind(instance,serial,specs,3)==XR_XIR_CALL_READY);
        XrXirDomainBudgetStats retried=xr_xir_domain_budget_stats(instance->domain);
        CHECK(retried.requested_call_bytes>failed.requested_call_bytes && retried.work>failed.work);
        binding_prefix_free(instance,serial);
        CHECK(runtime_live==baseline && runtime_bytes==baseline_bytes);
    }
    for (unsigned axis=0;axis<3;++axis) for (unsigned shortfall=0;shortfall<2;++shortfall) {
        XrXirInstanceConfig limited=config;
        if (axis==0) limited.requested_call_limit=required.requested_call_bytes-shortfall;
        if (axis==1) limited.work_limit=required.work-shortfall;
        if (axis==2) limited.metadata_limit=required.metadata_peak-shortfall;
        instance=NULL;serial=0;
        XrXirCallStatus status=binding_prefix(program,&limited,&instance,&serial);
        if (status==XR_XIR_CALL_READY) status=xr_xir_instance_generation_bind(instance,serial,specs,3);
        CHECK(status==(shortfall?XR_XIR_CALL_LIMIT:XR_XIR_CALL_READY));
        if (status!=XR_XIR_CALL_READY && instance && instance->prepared_generation)
            CHECK(!instance->prepared_generation->symbol_bindings);
        binding_prefix_free(instance,serial);
        CHECK(runtime_live==baseline && runtime_bytes==baseline_bytes);
    }
    xr_xir_compile_program_drop(program);
    CHECK(releases==1 && runtime_live==physical && runtime_bytes==physical_bytes);
    printf("binding prepare actual OOM sites%zu; same-owner retry; requested-call%llu/work%llu/metadata%llu six exact-minus-one controls; physical0\n",
        sites,(unsigned long long)required.requested_call_bytes,(unsigned long long)required.work,
        (unsigned long long)required.metadata_peak);
}
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--prepare-resources")) bindings_prepare_resources();
    else {CHECK(argc==1);bindings_normal();}
    instance_compile_report();native_fixture_owner_report();return 0;
}
