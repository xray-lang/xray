/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_atomic_a2_backend_cases.h - Bounded backend retry and physical ownership tests
 *
 * KEY CONCEPT:
 *   Real production bodies retain captured operands across exactly one CAS per poll.
 */
#ifndef XIR_ATOMIC_A2_BACKEND_CASES_H
#define XIR_ATOMIC_A2_BACKEND_CASES_H
#include "xir_atomic_a2_backend_hooks.h"
#include "xir/xxir_output.h"
#include <string.h>
typedef struct AtomicBackendBlock {void *pointer;size_t bytes;} AtomicBackendBlock;
static AtomicBackendBlock atomic_backend_blocks[512];
static size_t atomic_backend_live,atomic_backend_bytes,atomic_backend_attempts;
static size_t atomic_backend_fail_at=SIZE_MAX,atomic_backend_cas_attempts;
static bool atomic_backend_collide;
void *atomic_backend_allocate(size_t bytes) {
    if(atomic_backend_attempts++==atomic_backend_fail_at)return NULL;
    void *pointer=xr_malloc(bytes);CHECK(pointer);
    unsigned slot=0;while(slot<512 && atomic_backend_blocks[slot].pointer)++slot;
    CHECK(slot<512);atomic_backend_blocks[slot]=(AtomicBackendBlock){pointer,bytes};
    ++atomic_backend_live;atomic_backend_bytes+=bytes;return pointer;
}
void atomic_backend_free(void *pointer) {
    if(!pointer)return;
    unsigned slot=0;while(slot<512 && atomic_backend_blocks[slot].pointer!=pointer)++slot;
    CHECK(slot<512 && atomic_backend_live);
    atomic_backend_bytes-=atomic_backend_blocks[slot].bytes;--atomic_backend_live;
    atomic_backend_blocks[slot]=(AtomicBackendBlock){0};xr_free(pointer);
}
void *atomic_backend_calloc(size_t count,size_t bytes) {
    CHECK(!count || bytes<=SIZE_MAX/count);void *pointer=atomic_backend_allocate(count*bytes);
    if(pointer)memset(pointer,0,count*bytes);return pointer;
}
bool atomic_backend_cell_cas(_Atomic(uint64_t) *cell,uint64_t *expected,uint64_t desired,
    memory_order success,memory_order failure) {
    CHECK(failure!=memory_order_release && failure!=memory_order_acq_rel);
    ++atomic_backend_cas_attempts;
    if(atomic_backend_collide) {
        atomic_backend_collide=false;
        atomic_store_explicit(cell,UINT64_C(0x4000000000000000),memory_order_relaxed);
    }
    return atomic_compare_exchange_strong_explicit(cell,expected,desired,success,failure);
}
typedef struct AtomicBackendOutput {char bytes[128];size_t length;unsigned groups;} AtomicBackendOutput;
static XrXirOutputStatus atomic_backend_write(void *context,XrXirOutputStream stream,const char *bytes,size_t count) {
    AtomicBackendOutput *output=context;CHECK(stream==XR_XIR_STDOUT && count<=128-output->length);
    memcpy(output->bytes+output->length,bytes,count);output->length+=count;return XR_XIR_OUTPUT_OK;
}
static XrXirOutputStatus atomic_backend_group(void *context,const XrXirOutputGroup *group) {
    AtomicBackendOutput *output=context;CHECK(group && group->count==4);
    CHECK(group->values[0].type==XR_XIR_F64 && group->values[1].type==XR_XIR_F64 &&
        group->values[2].type==XR_XIR_F64 && group->values[3].type==XR_XIR_I64);
    ++output->groups;XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,atomic_backend_write,context,128};
    return xr_xir_output_render(&sink,group);
}
static void atomic_backend_case(XrXirProgram *program,uint32_t entry,bool cancel) {
    size_t live=atomic_backend_live,bytes=atomic_backend_bytes;
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    AtomicBackendOutput output={0};config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,atomic_backend_group,&output};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    atomic_backend_cas_attempts=0;atomic_backend_collide=true;
    XrXirInstanceResult result={0};unsigned polls=0;
    do {
        CHECK(++polls<4096);result=xr_xir_instance_poll_bounded(instance,1);
        CHECK(result.outcome.status==XR_XIR_CALL_READY);
    } while(atomic_backend_collide);
    CHECK(atomic_backend_cas_attempts==1 && !result.outcome.wake && !output.groups);
    if(cancel) {
        CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
        do {
            CHECK(++polls<4096);result=xr_xir_instance_poll_bounded(instance,1);
            CHECK(atomic_backend_cas_attempts==1 && !output.groups);
        } while(result.outcome.status==XR_XIR_CALL_READY);
        CHECK(result.outcome.status==XR_XIR_CALL_CANCELLED);
    } else {
        result=xr_xir_instance_poll_bounded(instance,1);
        CHECK(result.outcome.status==XR_XIR_CALL_READY && atomic_backend_cas_attempts==2);
        while(result.outcome.status==XR_XIR_CALL_READY) {
            CHECK(++polls<4096);result=xr_xir_instance_poll_bounded(instance,1);
        }
        CHECK(result.outcome.status==XR_XIR_CALL_RETURNED && atomic_backend_cas_attempts==3);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_F64 && !value.reserved && (uint64_t)value.payload==UINT64_C(0x400a000000000000));
        xr_xir_value_drop(&value);
        const char expected[]="2.0 2.25 3.25 111\n";
        CHECK(output.groups==1 && output.length==sizeof(expected)-1 && !memcmp(output.bytes,expected,sizeof(expected)-1));
    }
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(atomic_backend_live==live && atomic_backend_bytes==bytes);
}
static void atomic_backend_cases(XrXirProgram *program,uint32_t entry) {
    atomic_backend_case(program,entry,false);atomic_backend_case(program,entry,true);
    CHECK(!atomic_backend_live && !atomic_backend_bytes);
}
#endif
