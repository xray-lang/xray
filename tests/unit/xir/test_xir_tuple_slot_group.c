/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_tuple_slot_group.c - Actual publication, admission and cancellation boundaries
 */
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_source_program_compile_owner.h"
#include "xir_runtime_allocations.h"

typedef enum GroupMode {
    GROUP_NORMAL, GROUP_CANCEL_PREPARED, GROUP_CANCEL_COMMITTED, GROUP_RETAIN_LIMIT,
    GROUP_WORK_EXACT, GROUP_WORK_SHORT, GROUP_SCRATCH_EXACT, GROUP_SCRATCH_SHORT,
    GROUP_VALUE_EXACT, GROUP_VALUE_SHORT, GROUP_UNIT
} GroupMode;
typedef struct GroupWitness {
    XrXirInstance *instance;
    GroupMode mode;
    uint32_t published,released,ready,begins;
    uint64_t work_before,work_after,scratch_before,scratch_after;
    size_t prepare_allocations;
    XrXirCallStatus prepared;
} GroupWitness;
typedef struct GroupFrame {XrXirValue values[2];} GroupFrame;

static void group_trace(void *context,XrXirLifecycleEvent event,uint32_t index) {
    GroupWitness *w=context;XrXirInstance *instance=w->instance;
    if(event==XR_XIR_MODULE_BEGIN){CHECK(!index);++w->begins;return;}
    if(event==XR_XIR_MODULE_READY){CHECK(!index);++w->ready;return;}
    if(event==XR_XIR_SLOT_RELEASED){CHECK(index==2-w->released);++w->released;return;}
    CHECK(event==XR_XIR_SLOT_PUBLISHED && index==w->published && instance);
    /* The first public observation already sees the complete real group. */
    CHECK(instance->publication_count==3 && instance->published_counts[0]==3);
    for(uint32_t i=0;i<3;++i)CHECK(instance->published[i] && instance->publication_order[i]==i);
    CHECK(value_unit(instance->slots[1]));
    if(w->mode==GROUP_UNIT)CHECK(value_unit(instance->slots[0]) && value_unit(instance->slots[2]));
    else {
        XrXirValue field={0};
        CHECK(xr_xir_tuple_get(&instance->slots[0],0,&field)==XR_XIR_VALUE_OK && field.payload==1);
        xr_xir_value_drop(&field);
        CHECK(xr_xir_tuple_get(&instance->slots[2],1,&field)==XR_XIR_VALUE_OK && field.payload==4);
        xr_xir_value_drop(&field);
    }
    ++w->published;
    if(w->mode==GROUP_CANCEL_COMMITTED && w->published==1)
        CHECK(xr_xir_call_request_cancel(instance->call)==XR_XIR_CALL_CANCEL_REQUESTED);
}
static XrXirAction group_fault(XrXirCallStatus status) {
    return (XrXirAction){XR_XIR_ACTION_FAULT,0,NULL,0,{XR_XIR_I64,0,status},{0},0};
}
static XrXirAction group_initializer(XrXirCallView *view) {
    GroupWitness *w=(GroupWitness *)view->environment;GroupFrame *frame=view->state;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);CHECK(admission);
    bool unit=w->mode==GROUP_UNIT;
    if(!unit)for(uint32_t i=0;i<2;++i) {
        XrXirValue fields[]={{XR_XIR_I64,0,(int64_t)(i*2+1)},{XR_XIR_I64,0,(int64_t)(i*2+2)}};
        XrXirValueStatus made=xr_xir_tuple_new((XrXirType)256,fields,2,admission,&frame->values[i]);
        if(made!=XR_XIR_VALUE_OK)return group_fault(value_call_status(made));
    }
    /* A fresh activation owns each independent operation. Only lower its
     * existing limits; domain allocation history is never reset. */
    const uint64_t group_work=21+(unit?0:6);
    const uint64_t group_scratch=3*sizeof(XrXirValue)+(unit?0:sizeof(ValueAdmissionFrame));
    uint64_t value_live=xr_xir_domain_stats(admission->domain).live_bytes;
    if(w->mode==GROUP_WORK_EXACT || w->mode==GROUP_WORK_SHORT) {
        CHECK(group_work<=admission->work);admission->work=group_work-(w->mode==GROUP_WORK_SHORT);
    }
    if(w->mode==GROUP_SCRATCH_EXACT || w->mode==GROUP_SCRATCH_SHORT) {
        CHECK(group_scratch<=admission->scratch_bytes);
        admission->scratch_bytes=group_scratch-(w->mode==GROUP_SCRATCH_SHORT);
    }
    if(w->mode==GROUP_VALUE_EXACT || w->mode==GROUP_VALUE_SHORT) {
        CHECK(value_live+group_scratch<=admission->domain->limit);
        admission->domain->limit=value_live+group_scratch-(w->mode==GROUP_VALUE_SHORT);
    }
    XirObject *saturated=NULL;uint32_t references=0;
    if(w->mode==GROUP_RETAIN_LIMIT) {
        saturated=object_pointer(&frame->values[1]);
        references=atomic_load_explicit(&saturated->references,memory_order_relaxed);CHECK(references==1);
        atomic_store_explicit(&saturated->references,UINT32_MAX,memory_order_relaxed);
    }
    if(w->mode==GROUP_CANCEL_PREPARED)
        CHECK(xr_xir_call_request_cancel(view->activation)==XR_XIR_CALL_CANCEL_REQUESTED);
    w->work_before=admission->work;w->scratch_before=admission->scratch_bytes;
    size_t attempts=runtime_attempts;XrXirDomainStats before=xr_xir_domain_stats(admission->domain);
    w->prepared=xr_xir_instance_slot_group_init(view,0,3,unit?NULL:frame->values,unit?0:2);
    w->prepare_allocations=runtime_attempts-attempts;
    w->work_after=admission->work;w->scratch_after=admission->scratch_bytes;
    CHECK(w->scratch_before==w->scratch_after);
    CHECK(xr_xir_domain_stats(admission->domain).live_bytes==before.live_bytes);
    if(saturated) {
        CHECK(atomic_load_explicit(&saturated->references,memory_order_relaxed)==UINT32_MAX);
        CHECK(atomic_load_explicit(&object_pointer(&frame->values[0])->references,memory_order_relaxed)==1);
        atomic_store_explicit(&saturated->references,references,memory_order_relaxed);
    }
    if(w->prepared!=XR_XIR_CALL_READY) {
        CHECK(!w->published && !w->instance->publication_count && !w->instance->published_counts[0]);
        for(uint32_t i=0;i<3;++i)CHECK(!w->instance->published[i] && value_unit(w->instance->slots[i]));
        return group_fault(w->prepared);
    }
    CHECK(w->work_before-w->work_after==group_work);
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN};
}
static void group_release(XrXirCallView *view,XrXirCallStatus reason) {
    (void)reason;GroupFrame *frame=view->state;
    for(uint32_t i=0;i<2;++i)xr_xir_value_drop(&frame->values[i]);
}
static XrXirAction group_entry(XrXirCallView *view) {
    (void)view;return (XrXirAction){XR_XIR_ACTION_RETURN,0,NULL,0,{XR_XIR_I64,0,41},{0},0};
}
static XrXirArtifact *group_lower(const XrXirCompileContext *context,bool unit) {
    XrXirCallableParameter fields[]={{XR_XIR_I64,0},{XR_XIR_I64,0}};
    XrXirTypeNode node={.kind=XR_XIR_TYPE_TUPLE,.parameters=fields,.parameter_count=2};
    XrXirTypes types={&node,1,NULL,NULL};
    XrXirInstruction init[]={
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},1,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},2,{0}},
        {XR_XIR_TUPLE_NEW,(XrXirType)256,{0,2},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},3,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},4,{0}},
        {XR_XIR_TUPLE_NEW,(XrXirType)256,{2,2},{0},0,{0}},
        {XR_XIR_SLOT_GROUP_INIT,XR_XIR_UNIT,{4,2},{0},3,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirInstruction unit_init[]={
        {XR_XIR_SLOT_GROUP_INIT,XR_XIR_UNIT,{0},{0},3,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirInstruction entry[]={
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    const uint32_t operands[]={0,1,3,4,2,5};
    XrXirBlock blocks[]={{0,unit?2u:8u,0,0},{0,2,0,0}};
    XrXirFunction functions[]={
        {"init",4,NULL,0,XR_XIR_UNIT,&blocks[0],1,unit?unit_init:init,unit?2u:8u,unit?NULL:operands,unit?0:6},
        {"main",4,NULL,0,XR_XIR_I64,&blocks[1],1,entry,2,NULL,0}};
    XrXirSourceModule source_module={"root",4,NULL,0,0};
    XrXirFunctionIdentity identities[]={{.module=0},{.module=0,.exported=true}};
    XrXirSlot slots[]={{0,unit?XR_XIR_UNIT:(XrXirType)256,false},{0,XR_XIR_UNIT,false},
                      {0,unit?XR_XIR_UNIT:(XrXirType)256,false}};
    XrXirDeclarations declarations={&source_module,1,identities,slots,3,NULL,0,0,1,NULL};
    XrXirModule module={XR_XIR_BUILT,functions,2,&declarations,NULL,&types,NULL,XR_XIR_PROGRAM,NULL};
    XrXirArtifact *checked=NULL,*read=NULL,*closed=NULL,*lowered=NULL;XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_check(context,&module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);CHECK(xr_xir_compile_specialize(read,&closed,NULL)==XR_XIR_OK);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(read);xr_xir_compile_artifact_free(closed);return lowered;
}
static void group_run(XrXirArtifact *lowered,GroupMode mode,bool vm,size_t fail_at,size_t *attempts) {
    GroupWitness witness={.mode=mode};XrXirProgram *program=NULL;
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    XrXirVmBinding bindings[2]={0};
    XrXirCallEntry entries[]={
            {XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_UNIT,sizeof(GroupFrame),group_initializer,group_release,&witness,0,0},
            {XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_I64,0,group_entry,NULL,NULL,0,0}};
    if(vm)for(uint32_t i=0;i<2;++i)CHECK(xr_xir_compile_vm_bind(lowered,i,&bindings[i],&entries[i])==XR_XIR_OK);
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
            entries,2,module->declarations,{0},module->types,xr_xir_compile_program_proof(lowered)};
    CHECK(xr_xir_compile_program_seal(xr_xir_compile_artifact_context(lowered),&spec,&program)==XR_XIR_OK);
    size_t live=runtime_live,bytes=runtime_bytes;runtime_attempts=0;runtime_fail_at=fail_at;
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.trace=group_trace;config.trace_context=&witness;
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&witness.instance);
    if(status==XR_XIR_CALL_READY)status=xr_xir_instance_start(witness.instance,1,NULL,0);
    if(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll_bounded(witness.instance,1000000).outcome.status;
    *attempts=runtime_attempts;runtime_fail_at=SIZE_MAX;
    if(fail_at!=SIZE_MAX)CHECK(status==XR_XIR_CALL_OOM && runtime_attempts>fail_at);
    else if(mode==GROUP_CANCEL_PREPARED || mode==GROUP_CANCEL_COMMITTED)CHECK(status==XR_XIR_CALL_CANCELLED);
    else if(mode==GROUP_RETAIN_LIMIT || mode==GROUP_WORK_SHORT || mode==GROUP_SCRATCH_SHORT || mode==GROUP_VALUE_SHORT)
        CHECK(status==XR_XIR_CALL_LIMIT);
    else CHECK(status==XR_XIR_CALL_RETURNED);
    CHECK(witness.published==0 || witness.published==3);
    if(status!=XR_XIR_CALL_RETURNED && witness.instance) {
        CHECK(witness.released==witness.published);
        if(witness.begins) {
            CHECK(xr_xir_instance_state(witness.instance)==XR_XIR_INSTANCE_FAILED);
            size_t before=runtime_attempts;
            CHECK(xr_xir_instance_start(witness.instance,1,NULL,0)==status && before==runtime_attempts);
        }
    }
    if(mode==GROUP_CANCEL_PREPARED && fail_at==SIZE_MAX)CHECK(witness.prepare_allocations==3 && !witness.published);
    if(mode==GROUP_CANCEL_COMMITTED && fail_at==SIZE_MAX)CHECK(witness.published==3 && witness.released==3);
    if(witness.instance)CHECK(xr_xir_instance_free(witness.instance)==XR_XIR_CALL_READY);
    CHECK(runtime_live==live && runtime_bytes==bytes);xr_xir_compile_program_drop(program);
    CHECK(!runtime_live && !runtime_bytes);
}
int main(void) {
    const XrXirCompileContext *context=source_program_owner(UINT64_C(64)*1024*1024,128000000);
    XrXirArtifact *normal=group_lower(context,false),*unit=group_lower(context,true);
    size_t total=0;
    for(unsigned vm=0;vm<2;++vm) {
        size_t attempts=0;group_run(normal,GROUP_NORMAL,vm!=0,SIZE_MAX,&attempts);CHECK(attempts>8);
        for(size_t fault=0;fault<attempts;++fault) {size_t actual=0;group_run(normal,GROUP_NORMAL,vm!=0,fault,&actual);++total;}
        group_run(unit,GROUP_UNIT,vm!=0,SIZE_MAX,&attempts);
        for(size_t fault=0;fault<attempts;++fault) {size_t actual=0;group_run(unit,GROUP_UNIT,vm!=0,fault,&actual);++total;}
    }
    for(GroupMode mode=GROUP_CANCEL_PREPARED;mode<=GROUP_VALUE_SHORT;++mode) {
        size_t attempts=0;group_run(normal,mode,false,SIZE_MAX,&attempts);
    }
    xr_xir_compile_artifact_free(normal);xr_xir_compile_artifact_free(unit);source_program_owners_free();
    printf("Slot group: VM and native callbacks, actual OOM=%zu, complete first trace, Unit0payload, retain saturation, prepared/committed cancel, exact/minus-one work/scratch/value, physical0 PASS\n",total);
    return 0;
}
