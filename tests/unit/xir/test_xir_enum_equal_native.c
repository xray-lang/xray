/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_enum_equal_native.c - Real enum equality backend crossings
 */
#include "xir/xxir_program_packet_internal.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "xir_assert_compile_owner.h"
#include "xir_runtime_allocations.h"
#define ENUM_PROGRAM(i) XR_DATA const XrXirProgramSpec enum_equal_##i##_program; \
    XR_DATA const uint32_t enum_equal_##i##_indices[3]
ENUM_PROGRAM(0);ENUM_PROGRAM(1);ENUM_PROGRAM(2);ENUM_PROGRAM(3);ENUM_PROGRAM(4);ENUM_PROGRAM(5);
static const XrXirProgramSpec *const programs[]={&enum_equal_0_program,&enum_equal_1_program,
    &enum_equal_2_program,&enum_equal_3_program,&enum_equal_4_program,&enum_equal_5_program};
static const uint32_t *const indices[]={enum_equal_0_indices,enum_equal_1_indices,
    enum_equal_2_indices,enum_equal_3_indices,enum_equal_4_indices,enum_equal_5_indices};
typedef struct EnumEqualOwner {
    XrXirArtifact *artifact;
    XrXirCallEntry *entries;
    XrXirVmBinding *bindings;
    XrXirResumeEntry original[3];
    unsigned resumes[3];
    bool native[3];
} EnumEqualOwner;
static EnumEqualOwner *active;
static unsigned released;
static XrXirAction counted_run(XrXirCallView *view) {
    ++active->resumes[0];return active->original[0](view);
}
static XrXirAction counted_bridge(XrXirCallView *view) {
    ++active->resumes[1];return active->original[1](view);
}
static XrXirAction counted_probe(XrXirCallView *view) {
    ++active->resumes[2];return active->original[2](view);
}
static void owner_drop(void *pointer) {
    EnumEqualOwner *owner=pointer;xr_xir_compile_artifact_free(owner->artifact);
    xr_free(owner->entries);xr_free(owner->bindings);xr_free(owner);++released;
}
static XrXirProgram *mixed_program(unsigned index,unsigned mode,EnumEqualOwner **out) {
    const XrXirProgramSpec *native=programs[index];XrXirArtifact *checked=NULL;
    CHECK(xr_xir_compile_checked_read(assert_compile_context,native->packet.bytes,native->packet.length,&checked,NULL)==XR_XIR_OK);
    EnumEqualOwner *owner=xr_calloc(1,sizeof(*owner));CHECK(owner);
    CHECK(xr_xir_compile_lower(checked,&native->target, NULL,&owner->artifact,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    const XrXirModule *m=xr_xir_compile_artifact_module(owner->artifact);CHECK(m->function_count==native->entry_count);
    owner->entries=xr_calloc(m->function_count,sizeof(*owner->entries));
    owner->bindings=xr_calloc(m->function_count,sizeof(*owner->bindings));CHECK(owner->entries && owner->bindings);
    for(uint32_t f=0;f<m->function_count;++f){
        CHECK(xr_xir_compile_vm_bind(owner->artifact,f,&owner->bindings[f],&owner->entries[f])==XR_XIR_OK);
        bool use_native=mode==1 || (mode==2 ? f==indices[index][1] : f!=indices[index][1]);
        if(use_native)owner->entries[f]=native->entries[f];
        for(unsigned k=0;k<3;++k)if(f==indices[index][k])owner->native[k]=use_native;
    }
    XrXirResumeEntry wrappers[]={counted_run,counted_bridge,counted_probe};
    for(unsigned k=0;k<3;++k){uint32_t f=indices[index][k];owner->original[k]=owner->entries[f].resume;owner->entries[f].resume=wrappers[k];}
    CHECK(owner->native[0]==(mode!=2) && owner->native[1]==(mode!=3) && owner->native[2]==(mode!=2));
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,native->target,owner->entries,m->function_count,
        m->declarations,{owner,owner_drop},m->types,xir_program_packet_borrow(owner->artifact)};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(assert_compile_context,&spec,&program)==XR_XIR_OK);
    *out=owner;return program;
}
static void run_case(unsigned index,unsigned mode) {
    assert_compile_begin();EnumEqualOwner *owner=NULL;XrXirProgram *program=mixed_program(index,mode,&owner);active=owner;
    XrXirInstance *instance[2]={0};XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    for(unsigned i=0;i<2;++i)CHECK(xr_xir_instance_new(program,&config,&instance[i])==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    for(unsigned i=0;i<2;++i){
        CHECK(xr_xir_instance_start(instance[i],indices[index][0],NULL,0)==XR_XIR_CALL_READY);
        XrXirInstanceResult polled=xr_xir_instance_poll_bounded(instance[i],UINT64_MAX);
        if(polled.outcome.status!=XR_XIR_CALL_RETURNED)fprintf(stderr,"native case%u mode%u status%u\n",index,mode,polled.outcome.status);
        CHECK(polled.outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance[i],&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_BOOL && value.payload==1);xr_xir_value_drop(&value);
        if(!i){CHECK(owner->resumes[0] && owner->resumes[1] && owner->resumes[2]);
            printf("enum equality native case%u mode%u true; backend run/bridge/probe=%u/%u/%u actual resumes=%u/%u/%u\n",
                index,mode,owner->native[0],owner->native[1],owner->native[2],owner->resumes[0],owner->resumes[1],owner->resumes[2]);}
        CHECK(xr_xir_instance_free(instance[i])==XR_XIR_CALL_READY);
    }
    active=NULL;CHECK(!runtime_live && !runtime_bytes);CHECK(!assert_compile_extra_blocks() && !assert_compile_extra_bytes());assert_compile_end();
}
int main(void) {
    _Static_assert(XR_XIR_VALUE_ABI_VERSION==30 && XR_XIR_CALL_ABI_VERSION==35 && XR_XIR_PROGRAM_ABI_VERSION==36,"current physical ABI generations");
    for(unsigned index=0;index<6;++index)for(unsigned mode=1;mode<=3;++mode)run_case(index,mode);
    CHECK(released==18);puts("18 native/mixed primary outcomes plus18 second Instances; physical=0/0");return 0;
}
