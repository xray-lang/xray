/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */
#ifndef XIR_ATOMIC_A2_RUNTIME_FAULT_CASES_H
#define XIR_ATOMIC_A2_RUNTIME_FAULT_CASES_H
#include "xir_atomic_a2_backend_hooks.h"
#include "xir/xxir_output.h"
#include <string.h>
typedef struct AtomicBackendBlock {void *pointer;size_t bytes;} AtomicBackendBlock;
static AtomicBackendBlock atomic_backend_blocks[512];
static size_t atomic_backend_live,atomic_backend_bytes,atomic_backend_attempts;
static size_t atomic_fault_peak;
static size_t atomic_backend_fail_at=SIZE_MAX,atomic_backend_cas_attempts;
static bool atomic_backend_collide,atomic_fault_committed;
void *atomic_backend_allocate(size_t bytes) {
    if(atomic_backend_attempts++==atomic_backend_fail_at)return NULL;
    void *pointer=xr_malloc(bytes);CHECK(pointer);
    unsigned slot=0;while(slot<512 && atomic_backend_blocks[slot].pointer)++slot;
    CHECK(slot<512);atomic_backend_blocks[slot]=(AtomicBackendBlock){pointer,bytes};
    ++atomic_backend_live;atomic_backend_bytes+=bytes;
    if(atomic_backend_bytes>atomic_fault_peak)atomic_fault_peak=atomic_backend_bytes;
    return pointer;
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
    bool exchanged=atomic_compare_exchange_strong_explicit(cell,expected,desired,success,failure);
    if(exchanged && desired==UINT64_C(0x4000000000000000))atomic_fault_committed=true;
    return exchanged;
}

typedef struct AtomicFaultReport {
    XrXirCallStatus status;
    size_t attempts, cas, peak;
    XrXirInstanceState state;
    bool initialization_failed, instance_created, committed;
} AtomicFaultReport;
static XrXirInstanceConfig atomic_fault_config(void) {
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.metadata_limit=1048576; config.value_limit=1048576; config.call_limit=1048576;
    config.poll_limit=1000000; config.depth_limit=64;
    return config;
}
static XrXirCallStatus atomic_fault_drive(XrXirInstance *instance,uint32_t entry) {
    XrXirCallStatus status=xr_xir_instance_start(instance,entry,NULL,0);
    if(status!=XR_XIR_CALL_READY)return status;
    unsigned polls=0;XrXirInstanceResult result={0};
    do {
        CHECK(++polls<4096);result=xr_xir_instance_poll_bounded(instance,1);
    } while(result.outcome.status==XR_XIR_CALL_READY);
    CHECK(!result.outcome.wake);return result.outcome.status;
}
static void atomic_fault_text(const XrXirValue *value) {
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(value,&bytes,&length));
    CHECK(length==3 && !memcmp(bytes,"2.0",3));
}
static void atomic_fault_sticky(XrXirInstance *instance,XrXirCallStatus expected,uint32_t entry) {
    size_t attempts=atomic_backend_attempts;
    XrXirCallResult copy={0};
    CHECK(xr_xir_instance_copy_failure(instance,&copy)==expected);
    CHECK(copy.status==expected && !copy.value.type && !copy.value.reserved && !copy.value.payload);
    CHECK(!copy.wake && xr_xir_panic_empty(&copy.panic));xr_xir_call_result_drop(&copy);
    XrXirCallResult occupied={.status=XR_XIR_CALL_RETURNED,.value={XR_XIR_I64,0,91}},before=occupied;
    CHECK(xr_xir_instance_copy_failure(instance,&occupied)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!memcmp(&occupied,&before,sizeof(before)));xr_xir_call_result_drop(&occupied);
    XrXirValue out={XR_XIR_I64,0,37},old=out;
    CHECK(xr_xir_instance_take_result(instance,&out)==XR_XIR_CALL_BAD_STATE);
    CHECK(!memcmp(&out,&old,sizeof(old)));xr_xir_value_drop(&out);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==expected);
    CHECK(xr_xir_instance_poll_bounded(instance,1).outcome.status==expected);
    CHECK(atomic_backend_attempts==attempts);
}
static AtomicFaultReport atomic_fault_probe(XrXirProgram *program,const uint32_t entries[2],
    const XrXirInstanceConfig *config,size_t fail_at,bool inspect) {
    CHECK(!atomic_backend_live && !atomic_backend_bytes);
    size_t compiler_blocks=source_program_compile_live,compiler_bytes=source_program_compile_bytes;
    atomic_fault_peak=0;atomic_backend_attempts=0;atomic_backend_cas_attempts=0;atomic_backend_fail_at=fail_at;
    atomic_backend_collide=false;atomic_fault_committed=false;XrXirInstance *instance=NULL;
    AtomicFaultReport report={0};report.status=xr_xir_instance_new(program,config,&instance);
    CHECK((report.status==XR_XIR_CALL_READY)==(instance!=NULL));
    report.instance_created=instance!=NULL;
    if(instance)report.status=atomic_fault_drive(instance,entries[0]);
    report.peak=atomic_fault_peak;report.attempts=atomic_backend_attempts;report.cas=atomic_backend_cas_attempts;report.committed=atomic_fault_committed;
    atomic_backend_fail_at=SIZE_MAX;
    if(instance) {
        report.state=xr_xir_instance_state(instance);
        report.initialization_failed=report.state==XR_XIR_INSTANCE_FAILED;
        if(report.initialization_failed)atomic_fault_sticky(instance,report.status,entries[0]);
        if(report.status==XR_XIR_CALL_RETURNED) {
            CHECK(report.cas==2 && report.committed);XrXirValue text={0};
            CHECK(xr_xir_instance_take_result(instance,&text)==XR_XIR_CALL_RETURNED);
            atomic_fault_text(&text);xr_xir_value_drop(&text);
        } else {
            XrXirValue out={0};CHECK(xr_xir_instance_take_result(instance,&out)==XR_XIR_CALL_BAD_STATE);
            CHECK(!out.type && !out.reserved && !out.payload);
        }
        if(inspect && xr_xir_instance_state(instance)==XR_XIR_INSTANCE_READY) {
            CHECK(atomic_fault_drive(instance,entries[1])==XR_XIR_CALL_RETURNED);
            XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
            CHECK(value.type==XR_XIR_F64 && !value.reserved);
            CHECK((uint64_t)value.payload==(report.committed?UINT64_C(0x4000000000000000):UINT64_C(0x3ff8000000000000)));
            CHECK(atomic_backend_cas_attempts==report.cas);xr_xir_value_drop(&value);
        }
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    CHECK(!atomic_backend_live && !atomic_backend_bytes);
    CHECK(source_program_compile_live==compiler_blocks && source_program_compile_bytes==compiler_bytes);
    return report;
}
static void atomic_fault_ordinals(XrXirProgram *program,const uint32_t entries[2]) {
    XrXirInstanceConfig config=atomic_fault_config();
    AtomicFaultReport baseline=atomic_fault_probe(program,entries,&config,SIZE_MAX,true);
    CHECK(baseline.status==XR_XIR_CALL_RETURNED && baseline.attempts && baseline.attempts<512);
    size_t created=0,start=0,sticky=0,precommit=0,postcommit=0;
    for(size_t ordinal=0;ordinal<baseline.attempts;++ordinal) {
        AtomicFaultReport r=atomic_fault_probe(program,entries,&config,ordinal,true);
        if(r.status!=XR_XIR_CALL_OOM)fprintf(stderr,"runtime fault ordinal=%zu/%zu attempts=%zu status=%u CAS=%zu init=%d\n",ordinal,baseline.attempts,r.attempts,r.status,r.cas,r.initialization_failed);
        CHECK(r.attempts>ordinal && r.status==XR_XIR_CALL_OOM);
        if(!r.instance_created)++created;
        else if(r.state==XR_XIR_INSTANCE_NEW)++start;
        else if(r.initialization_failed)++sticky;
        else if(!r.committed)++precommit;
        else {CHECK(r.cas==2);++postcommit;}
    }
    CHECK(created && start && sticky && precommit && postcommit);
    CHECK(created+start+sticky+precommit+postcommit==baseline.attempts);
    printf("backend runtime OOM sites=%zu new=%zu start=%zu sticky=%zu precommit=%zu postcommit=%zu peak=%zu physical0\n",
        baseline.attempts,created,start,sticky,precommit,postcommit,baseline.peak);
}
static uint64_t atomic_fault_axis_get(const XrXirInstanceConfig *config,unsigned axis) {
    if(axis==0)return config->metadata_limit;
    if(axis==1)return config->value_limit;
    if(axis==2)return config->call_limit;
    if(axis==3)return config->poll_limit;
    CHECK(axis==4);return config->depth_limit;
}
static void atomic_fault_axis_set(XrXirInstanceConfig *config,unsigned axis,uint64_t value) {
    if(axis==0)config->metadata_limit=value;
    else if(axis==1)config->value_limit=value;
    else if(axis==2)config->call_limit=value;
    else if(axis==3)config->poll_limit=value;
    else {CHECK(axis==4 && value<=64);config->depth_limit=(uint32_t)value;}
}
static void atomic_fault_resources(XrXirProgram *program,const uint32_t entries[2]) {
    for(unsigned axis=0;axis<5;++axis) {
        XrXirInstanceConfig config=atomic_fault_config();uint64_t low=0,high=atomic_fault_axis_get(&config,axis);
        while(low+1<high) {
            uint64_t mid=low+(high-low)/2;atomic_fault_axis_set(&config,axis,mid);
            AtomicFaultReport r=atomic_fault_probe(program,entries,&config,SIZE_MAX,false);
            CHECK(r.status==XR_XIR_CALL_RETURNED || r.status==XR_XIR_CALL_LIMIT);
            if(r.status==XR_XIR_CALL_RETURNED)high=mid;else low=mid;
        }
        atomic_fault_axis_set(&config,axis,high);
        CHECK(atomic_fault_probe(program,entries,&config,SIZE_MAX,false).status==XR_XIR_CALL_RETURNED);
        atomic_fault_axis_set(&config,axis,high-1);
        CHECK(atomic_fault_probe(program,entries,&config,SIZE_MAX,false).status==XR_XIR_CALL_LIMIT);
        printf("backend runtime axis%u exact=%llu minus1=LIMIT physical0\n",axis,(unsigned long long)high);
    }
}
static void atomic_fault_escaped(XrXirProgram *program,const uint32_t entries[2]) {
    XrXirInstanceConfig config=atomic_fault_config();XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    /* Only the Instance retains Program code and metadata while the entry runs. */
    xr_xir_compile_program_drop(program);program=NULL;
    CHECK(atomic_fault_drive(instance,entries[0])==XR_XIR_CALL_RETURNED);
    XrXirValue text={0};CHECK(xr_xir_instance_take_result(instance,&text)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(atomic_backend_live && atomic_backend_bytes);atomic_fault_text(&text);
    xr_xir_value_drop(&text);CHECK(!text.type && !text.reserved && !text.payload);
    xr_xir_value_drop(&text);xr_xir_compile_program_drop(program);
    CHECK(!atomic_backend_live && !atomic_backend_bytes);
}
static void atomic_fault_cases(XrXirProgram *program,const uint32_t entries[2]) {
    atomic_fault_ordinals(program,entries);atomic_fault_resources(program,entries);
    atomic_fault_escaped(program,entries);
}
#endif
