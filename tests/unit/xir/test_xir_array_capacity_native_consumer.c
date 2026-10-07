/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_array_capacity_native_consumer.c - Runtime-only independent generated-C capacity checks
 *
 * KEY CONCEPT:
 *   Native entries must preserve root ordering, aliases and escaped ownership.
 */
#include "xir/xxir_program.h"
#include "xir/xxir_array.h"
#include "xir/xxir_value.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_instance_compile_observer.h"
#include "xir_source_fixture_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_array_capacity_native_oracles.h"
#include "capacity_native_contract_generated.h"
static XrXirOutputStatus capacity_native_output(void *context,const XrXirOutputGroup *group) {
    unsigned *count=context;CHECK(group&&group->stream==XR_XIR_STDOUT&&group->line&&group->count==1&&group->values);
    const XrXirValue *value=&group->values[0];CHECK(*count<2);
    if(!*count)CHECK(value->type==XR_XIR_I64&&!value->reserved&&value->payload==17);
    else{const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(value,&bytes,&length));
        CHECK(length==10&&!memcmp(bytes,"cap\0native",10));}
    ++*count;return XR_XIR_OUTPUT_OK;
}
static XrXirInstanceResult capacity_native_poll(XrXirInstance *instance) {
    for(unsigned n=0;n<65536;++n){
        XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1);
        if(result.outcome.status!=XR_XIR_CALL_READY)return result;
    }
    CHECK(false);return (XrXirInstanceResult){0};
}
static XrXirValue capacity_native_run(XrXirInstance *instance,unsigned named) {
    CHECK(named<CAP_NATIVE_COUNT);
    CHECK(xr_xir_instance_start(instance,capacity_native_entries[named],NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult result=capacity_native_poll(instance);
    if(result.outcome.status!=XR_XIR_CALL_RETURNED)fprintf(stderr,"native capacity %s status%u fault%u\n",
        capacity_native_names[named],result.outcome.status,result.outcome.panic.detail.code);
    CHECK(result.outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue occupied={XR_XIR_I64,0,99},preserved=occupied;
    CHECK(xr_xir_instance_take_result(instance,&occupied)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!memcmp(&occupied,&preserved,sizeof(occupied)));
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static void capacity_native_scalar(XrXirInstance *instance,unsigned named,XrXirType type,int64_t expected) {
    XrXirValue value=capacity_native_run(instance,named);
    CHECK(value.type==(uint32_t)type&&!value.reserved&&value.payload==expected);xr_xir_value_drop(&value);
}
static void capacity_native_escape_diagnostic(const XrXirValue *value,const char *label,
    XrXirValueAdmission *admission,XrXirValueStatus length_status,int64_t length,
    XrXirValueStatus capacity_status,int64_t capacity) {
    fprintf(stderr,"NATIVE_CAPACITY_ESCAPE case=%s type=%u reserved=%u payload=%lld domain=%p lenStatus=%u len=%lld capStatus=%u cap=%lld work=%llu\n",
        label,value->type,value->reserved,(long long)value->payload,(void *)admission->domain,
        (unsigned)length_status,(long long)length,(unsigned)capacity_status,(long long)capacity,
        (unsigned long long)admission->work);
    if(length_status!=XR_XIR_VALUE_OK||length<0||length>16)return;
    for(int64_t i=0;i<length;++i){
        XrXirValue element={0};XrXirFaultDetail fault={0};const char *bytes=NULL;size_t count=0;
        XrXirValueStatus status=xr_xir_array_get(value,i,admission,&element,&fault);
        const bool string=status==XR_XIR_VALUE_OK&&xr_xir_string_view(&element,&bytes,&count);
        fprintf(stderr,"NATIVE_CAPACITY_ESCAPE_ITEM case=%s index=%lld status=%u type=%u reserved=%u fault=%u string=%u bytes=%zu hex=",
            label,(long long)i,(unsigned)status,element.type,element.reserved,fault.code,(unsigned)string,count);
        if(string)for(size_t n=0;n<count&&n<32;++n)fprintf(stderr,"%02x",(unsigned)(unsigned char)bytes[n]);
        fputc('\n',stderr);xr_xir_value_drop(&element);
    }
}
static void capacity_native_array(const XrXirValue *value,bool changed,const char *label) {
    CHECK(xr_xir_value_valid(value));
    XrXirValueAdmission admission={xr_xir_value_arena(value),NULL,NULL,NULL,100000,1048576};
    int64_t rejected=-17;const uint64_t initial_work=admission.work;
    XrXirValueStatus rejected_status=xr_xir_array_capacity(value,&admission,&rejected);
    if(rejected_status!=XR_XIR_VALUE_BAD_ARGUMENT||rejected!=-17||admission.work!=initial_work)
        fprintf(stderr,"NATIVE_CAPACITY_ESCAPE_NULL_DOMAIN case=%s status=%u cap=%lld work=%llu expected=BAD_ARGUMENT/-17/%llu\n",
            label,(unsigned)rejected_status,(long long)rejected,(unsigned long long)admission.work,
            (unsigned long long)initial_work);
    CHECK(rejected_status==XR_XIR_VALUE_BAD_ARGUMENT&&rejected==-17&&admission.work==initial_work);
    admission.domain=object_pointer(value)->domain;CHECK(admission.domain);
    int64_t length=-1,capacity=-1;
    XrXirValueStatus length_status=xr_xir_array_len(value,&admission,&length);
    XrXirValueStatus capacity_status=xr_xir_array_capacity(value,&admission,&capacity);
    if(length_status!=XR_XIR_VALUE_OK||length!=(changed?2:1)||
        capacity_status!=XR_XIR_VALUE_OK||capacity<(changed?11:1))
        capacity_native_escape_diagnostic(value,label,&admission,length_status,length,capacity_status,capacity);
    CHECK(length_status==XR_XIR_VALUE_OK&&length==(changed?2:1));
    CHECK(capacity_status==XR_XIR_VALUE_OK&&capacity>=(changed?11:1));
    static const char *const changed_text[]={"fresh\0z","tail"};
    static const size_t changed_length[]={7,4};
    for(int64_t i=0;i<length;++i){
        XrXirValue element={0};XrXirFaultDetail fault={0};const char *bytes=NULL;size_t bytes_count=0;
        XrXirValueStatus status=xr_xir_array_get(value,i,&admission,&element,&fault);
        const bool string=status==XR_XIR_VALUE_OK&&xr_xir_string_view(&element,&bytes,&bytes_count);
        if(!string||bytes_count!=(changed?changed_length[i]:6)||
            memcmp(bytes,changed?changed_text[i]:"seed\0x",bytes_count))
            capacity_native_escape_diagnostic(value,label,&admission,length_status,length,capacity_status,capacity);
        CHECK(status==XR_XIR_VALUE_OK&&string);
        CHECK(bytes_count==(changed?changed_length[i]:6)&&!memcmp(bytes,changed?changed_text[i]:"seed\0x",bytes_count));
        xr_xir_value_drop(&element);
    }
}
static void capacity_native_path_start(XrXirInstance *instance,unsigned index,uint32_t entry) {
    XrXirCallStatus status=xr_xir_instance_start(instance,entry,NULL,0);
    if(status!=XR_XIR_CALL_READY)fprintf(stderr,
        "NATIVE_CAPACITY_PATH_START instance=%u entry=%u status=%u state=%u exported=%u programEntry=%u\n",
        index,entry,(unsigned)status,(unsigned)xr_xir_instance_state(instance),
        capacity_native_path_program.declarations->functions[entry].exported,
        capacity_native_path_program.declarations->entry_function);
    CHECK(status==XR_XIR_CALL_READY);
}
static void capacity_native_path_cases(const XrXirCompileContext *context) {
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(context,&capacity_native_path_program,&program)==XR_XIR_OK);
    XrXirInstance *instances[2]={0};
    for(unsigned i=0;i<2;++i){XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY);}
    xr_xir_compile_program_drop(program);program=NULL;
    for(unsigned i=0;i<2;++i){
        capacity_native_path_start(instances[i],i,3);
        CHECK(capacity_native_poll(instances[i]).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instances[i],&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_I64&&!value.reserved&&!value.payload);xr_xir_value_drop(&value);
        capacity_native_path_start(instances[i],i,1);
        CHECK(capacity_native_poll(instances[i]).outcome.status==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instances[i],&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_BOOL&&!value.reserved&&value.payload==1);xr_xir_value_drop(&value);
        capacity_native_path_start(instances[i],i,2);
        XrXirInstanceResult result=capacity_native_poll(instances[i]);
        CHECK(result.outcome.status==XR_XIR_CALL_BOUNDS&&result.outcome.panic.detail.code==430&&
            result.outcome.panic.detail.index==1&&result.outcome.panic.detail.length==1);
        CHECK(!result.outcome.value.type&&!result.outcome.value.reserved&&!result.outcome.value.payload);
        CHECK(xr_xir_instance_take_result(instances[i],&value)==XR_XIR_CALL_BAD_STATE);
        CHECK(!value.type&&!value.reserved&&!value.payload);
        CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);}
    CHECK(!runtime_live&&!runtime_bytes);
}
int main(void) {
    instance_compile_zero();CHECK(!runtime_live&&!runtime_bytes);
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context,&capacity_native_contract_program,&program)==XR_XIR_OK);
    unsigned outputs[2]={0};XrXirInstance *instances[2]={0};
    for(unsigned i=0;i<2;++i){XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,capacity_native_output,&outputs[i]};
        CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY);}
    CHECK(instances[0]->domain!=instances[1]->domain);xr_xir_compile_program_drop(program);program=NULL;
    for(unsigned i=0;i<2;++i){capacity_native_scalar(instances[i],CAP_NATIVE_ENTRY,XR_XIR_I64,0);CHECK(outputs[i]==2);
        capacity_native_scalar(instances[i],CAP_NATIVE_SHAPE,XR_XIR_BOOL,1);
        capacity_native_scalar(instances[i],CAP_NATIVE_PATH,XR_XIR_BOOL,1);
        capacity_native_scalar(instances[i],CAP_NATIVE_NEGATIVE_NEW,XR_XIR_I64,422);
        capacity_native_scalar(instances[i],CAP_NATIVE_NEGATIVE_RESERVE,XR_XIR_I64,422);}
    XrXirValue values[3]={capacity_native_run(instances[0],CAP_NATIVE_RESERVE),
        capacity_native_run(instances[0],CAP_NATIVE_SNAPSHOT),capacity_native_run(instances[1],CAP_NATIVE_SNAPSHOT)};
    CHECK(values[0].payload!=values[2].payload&&values[1].payload!=values[2].payload);
    capacity_native_scalar(instances[0],CAP_NATIVE_TRACE,XR_XIR_I64,12);
    capacity_native_scalar(instances[1],CAP_NATIVE_TRACE,XR_XIR_I64,0);
    capacity_native_scalar(instances[0],CAP_NATIVE_MUTATE,XR_XIR_I64,0);
    CHECK(xr_xir_instance_start(instances[1],capacity_native_entries[CAP_NATIVE_OVERFLOW],NULL,0)==XR_XIR_CALL_READY);
    CHECK(capacity_native_poll(instances[1]).outcome.status==XR_XIR_CALL_LIMIT);
    XrXirValue empty={0};CHECK(xr_xir_instance_take_result(instances[1],&empty)==XR_XIR_CALL_BAD_STATE);
    CHECK(!empty.type&&!empty.reserved&&!empty.payload);
    for(unsigned i=0;i<2;++i)CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);
    CHECK(runtime_live&&runtime_bytes);
    capacity_native_array(&values[0],true,"reserveResult");
    capacity_native_array(&values[1],true,"firstSnapshot");
    capacity_native_array(&values[2],false,"secondSnapshot");
    for(unsigned i=0;i<3;++i)xr_xir_value_drop(&values[i]);
    CHECK(!runtime_live&&!runtime_bytes);
    capacity_native_path_cases(&owner.context);
    source_fixture_owner_free(&owner);instance_compile_report();
    puts("NATIVE_CAPACITY_COMPLETE runtimeOnly=1 instances=2 typed17_NUL10=4 lowerBounds=9/11 aliases=3 trace12/0 negative422=4 overflowLIMIT=1 occupiedPreserved=1 escapedNullDomainBAD_ARGUMENT=3 BuiltPathInstances=2 lower13=2 BOUNDS430_1/1=2 physical=0/0");return 0;
}
