/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_tuple_slot_publication_gap.c - Expose sequential publication before a real second admit failure
 */
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_source_program_compile_owner.h"
#include "xir_runtime_allocations.h"
typedef struct GapTrace {unsigned published,released,begins,ready;size_t injected;} GapTrace;
static void trace_gap(void *context,XrXirLifecycleEvent event,uint32_t index) {
    GapTrace *trace=context;
    if(event==XR_XIR_MODULE_BEGIN){CHECK(!index);++trace->begins;}
    else if(event==XR_XIR_MODULE_READY){CHECK(!index);++trace->ready;}
    else if(event==XR_XIR_SLOT_PUBLISHED){
        CHECK(index==0 && !trace->published);++trace->published;
        /* Both Tuple objects already exist in SSA before either SLOT_INIT.
         * Fail the next actual allocator call: slot 1's graph-admission stack. */
        trace->injected=runtime_attempts;runtime_fail_at=runtime_attempts;
    } else {CHECK(event==XR_XIR_SLOT_RELEASED && index==0);++trace->released;}
}
static XrXirArtifact *gap_lower(const XrXirCompileContext *context) {
    const XrXirType tuple=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirCallableParameter fields[]={{XR_XIR_I64,0},{XR_XIR_I64,0}};
    XrXirTypeNode node={.kind=XR_XIR_TYPE_TUPLE,.parameters=fields,.parameter_count=2};
    XrXirTypes types={&node,1,NULL,NULL};
    XrXirInstruction init[]={
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},1,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},2,{0}},
        {XR_XIR_TUPLE_NEW,tuple,{0,2},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},3,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},4,{0}},
        {XR_XIR_TUPLE_NEW,tuple,{2,2},{0},0,{0}},
        {XR_XIR_SLOT_INIT,XR_XIR_UNIT,{2},{0},0,{0}},
        {XR_XIR_SLOT_INIT,XR_XIR_UNIT,{5},{0},1,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirInstruction entry[]={
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    const uint32_t operands[]={0,1,3,4};
    XrXirBlock blocks[]={{0,9,0,0},{0,2,0,0}};
    XrXirFunction functions[]={
        {"init",4,NULL,0,XR_XIR_UNIT,&blocks[0],1,init,9,operands,4},
        {"main",4,NULL,0,XR_XIR_I64,&blocks[1],1,entry,2,NULL,0}};
    XrXirSourceModule source_module={"root",4,NULL,0,0};
    XrXirFunctionIdentity identities[]={{.module=0},{.module=0,.exported=true}};
    XrXirSlot slots[]={{0,tuple,false},{0,tuple,false}};
    XrXirDeclarations declarations={&source_module,1,identities,slots,2,NULL,0,0,1,NULL};
    XrXirModule module={XR_XIR_BUILT,functions,2,&declarations,NULL,&types,NULL,XR_XIR_PROGRAM,NULL};
    XrXirArtifact *checked=NULL,*read=NULL,*closed=NULL,*lowered=NULL;
    XrXirDiagnostic diagnostic={0};XrXirCheckedPacket packet={0};
    XrXirStatus status=xr_xir_compile_check(context,&module,&checked,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"gap Check=%u f=%u b=%u i=%u\n",status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(read,&closed,NULL)==XR_XIR_OK);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(read);xr_xir_compile_artifact_free(closed);return lowered;
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--require-atomic")));
    const XrXirCompileContext *context=source_program_owner(UINT64_C(64)*1024*1024,128000000);
    XrXirArtifact *artifact=gap_lower(context);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&artifact,&program)==XR_XIR_OK && !artifact && program);
    XrXirInstanceConfig config;XrXirInstance *instance=NULL;GapTrace trace={0};
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.trace=trace_gap;config.trace_context=&trace;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,1,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1000000);
    runtime_fail_at=SIZE_MAX;
    CHECK(result.outcome.status==XR_XIR_CALL_OOM && trace.published==1 && trace.released==1 && trace.begins==1 && !trace.ready);
    CHECK(runtime_attempts==trace.injected+1 && !instance->publication_count && !instance->published[0] && !instance->published[1]);
    CHECK(xr_xir_instance_state(instance)==XR_XIR_INSTANCE_FAILED);
    size_t attempts=runtime_attempts;
    CHECK(xr_xir_instance_start(instance,1,NULL,0)==XR_XIR_CALL_OOM && runtime_attempts==attempts);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    CHECK(!runtime_live && !runtime_bytes);source_program_owners_free();
    printf("EXPOSED: prepared Tuple2 + Tuple2; second SLOT_INIT admission malloc ordinal=%zu fails OOM; published=1/released=1/ready=0; sticky/physical0\n",trace.injected);
    if(argc==2){fprintf(stderr,"ATOMIC GROUP REQUIREMENT FAILED: prepare failure emitted one publication event\n");return 1;}
    return 0;
}
