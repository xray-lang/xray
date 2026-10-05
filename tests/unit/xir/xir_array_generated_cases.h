/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_generated_cases.h - Source-backed Array execution and ownership qualification
 *
 * KEY CONCEPT:
 *   VM and native consume one Source-derived descriptor with independent expected values.
 */
#ifndef XIR_ARRAY_GENERATED_CASES_H
#define XIR_ARRAY_GENERATED_CASES_H
#include "xir/xxir_array.h"
enum { ARRAY_STRINGS,ARRAY_EMPTY,ARRAY_INTEGERS,ARRAY_NESTED,ARRAY_ALIAS_INITIAL,
       ARRAY_ALIAS_CHANGED,ARRAY_DEFAULT,ARRAY_APPEND,ARRAY_APPEND_MANAGED,ARRAY_MANAGED_VALUES,ARRAY_REPEATED_READ,ARRAY_ENTRY_COUNT };
typedef struct ArrayProgramEntries { uint32_t entries[ARRAY_ENTRY_COUNT]; } ArrayProgramEntries;
static const char *const array_entry_names[ARRAY_ENTRY_COUNT]={
    "strings","empty","integers","nested","aliasInitial","aliasChanged","defaultCount","appendNine","appendManaged","managedValues","repeatedRead"};
static XrXirInstance *array_instance(XrXirProgram *program) {
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.metadata_limit=UINT64_C(1)*1024*1024;config.value_limit=UINT64_C(1)*1024*1024;
    config.call_limit=UINT64_C(1)*1024*1024;config.poll_limit=1000000;config.depth_limit=64;
    XrXirInstance *instance=NULL;
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
    CHECK((status==XR_XIR_CALL_READY && instance) || (status==XR_XIR_CALL_OOM && !instance));
    return instance;
}
static XrXirCallResult array_drive(XrXirInstance *instance,uint32_t entry,const XrXirValue *arg) {
    XrXirCallStatus status=xr_xir_instance_start(instance,entry,arg,arg?1:0);
    if(status!=XR_XIR_CALL_READY)return (XrXirCallResult){.status=status};
    XrXirInstanceResult result;
    do {result=xr_xir_instance_poll_bounded(instance,16);}while(result.outcome.status==XR_XIR_CALL_READY);
    CHECK(!result.outcome.wake);return result.outcome;
}
static XrXirValueAdmission array_admission(const XrXirValue *value) {
    XirObject *object=(XirObject *)(uintptr_t)value->payload;CHECK(object && object->arena && object->domain);
    return (XrXirValueAdmission){object->arena,object->domain,NULL,NULL,1000000,UINT64_C(1)*1024*1024};
}
static int64_t array_length(const XrXirValue *value) {
    XrXirValueAdmission admission=array_admission(value);int64_t length=-1;
    CHECK(xr_xir_array_len(value,&admission,&length)==XR_XIR_VALUE_OK);return length;
}
static XrXirValue array_element(const XrXirValue *value,int64_t index) {
    XrXirValueAdmission admission=array_admission(value);XrXirValue result={0};XrXirFaultDetail fault={0};
    CHECK(xr_xir_array_get(value,index,&admission,&result,&fault)==XR_XIR_VALUE_OK && xr_xir_fault_empty(fault));
    return result;
}
static void array_string(const XrXirValue *value,const char *expected,size_t size) {
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(value,&bytes,&length) && length==size && !memcmp(bytes,expected,size));
}
static void array_contents(const XrXirValue *value,unsigned which) {
    const int64_t lengths[]={2,0,2,1,3,3};CHECK(which<6 && array_length(value)==lengths[which]);
    for(int64_t i=0;i<lengths[which];++i) {
        XrXirValue element=array_element(value,i);
        if(which==ARRAY_INTEGERS)CHECK(element.type==XR_XIR_I64 && element.payload==42);
        else if(which==ARRAY_STRINGS)array_string(&element,"a\xe4\xb8\x96",4);
        else {
            CHECK(array_length(&element)==2);
            for(int64_t j=0;j<2;++j) {
                XrXirValue inner=array_element(&element,j);
                if(which==ARRAY_NESTED)array_string(&inner,"a\xe4\xb8\x96",4);
                else CHECK(inner.type==XR_XIR_I64 && inner.payload==
                    (which==ARRAY_ALIAS_CHANGED && !i && !j?99:42));
                xr_xir_value_drop(&inner);
            }
        }
        xr_xir_value_drop(&element);
    }
}
static XrXirValue array_success(XrXirProgram *program,uint32_t entry,const XrXirValue *arg) {
    CHECK(runtime_fail_at==SIZE_MAX);XrXirInstance *instance=array_instance(program);CHECK(instance);
    XrXirCallResult result=array_drive(instance,entry,arg);CHECK(result.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_panic_empty(&result.panic));XrXirValue retained={0};
    CHECK(xr_xir_instance_take_result(instance,&retained)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,arg,arg?1:0)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);return retained;
}
static void array_fault_scan(XrXirProgram *program,uint32_t entry,unsigned which,int64_t count) {
    size_t compiler_live=array_program_compile_live,compiler_bytes=array_program_compile_bytes;
    XrXirValue arg={XR_XIR_I64,0,count};const XrXirValue *input=which==ARRAY_DEFAULT || which==ARRAY_APPEND?&arg:NULL;
    /* Preserve the bounded original 1..127 allocation-ordinal search. Each
     * iteration rebuilds the entire runtime operation, including its owners. */
    bool complete=false;size_t sites=0;
    for(size_t fail=0;fail<127;++fail) {
        CHECK(!runtime_live && !runtime_bytes);runtime_attempts=0;runtime_fail_at=fail;
        XrXirInstance *instance=array_instance(program);XrXirCallResult result={.status=XR_XIR_CALL_OOM};
        if(instance)result=array_drive(instance,entry,input);
        size_t attempts=runtime_attempts;runtime_fail_at=SIZE_MAX;
        if(attempts>fail) {
            if(result.status!=XR_XIR_CALL_OOM)fprintf(stderr,"Array OOM which=%u fail=%zu attempts=%zu status=%u\n",which,fail,attempts,result.status);
            CHECK(result.status==XR_XIR_CALL_OOM);
            if(instance) {
                XrXirValue empty={0},sentinel={XR_XIR_I64,0,91},out=sentinel;
                CHECK(xr_xir_instance_take_result(instance,&empty)==XR_XIR_CALL_BAD_STATE);
                CHECK(!empty.type && !empty.reserved && !empty.payload);
                XrXirCallStatus refused=xr_xir_instance_take_result(instance,&out);
                XrXirCallStatus expected=(instance->state==XR_XIR_INSTANCE_FAILED || !instance->call)?XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_BAD_ARGUMENT;
                CHECK(refused==expected);
                CHECK(!memcmp(&out,&sentinel,sizeof(out)));
            }
        } else {
            if(which==ARRAY_DEFAULT && count<0) {
                CHECK(result.status==XR_XIR_CALL_NUMERIC_RANGE && result.panic.detail.code==422);
            } else {
                CHECK(result.status==XR_XIR_CALL_RETURNED);XrXirValue value={0};
                CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
                if(which<6)array_contents(&value,which);
                else CHECK(value.type==XR_XIR_I64 && value.payload==(which==ARRAY_DEFAULT?count:9));
                xr_xir_value_drop(&value);
            }
            complete=true;sites=fail;
        }
        if(instance)CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        CHECK(!runtime_live && !runtime_bytes && array_program_compile_live==compiler_live && array_program_compile_bytes==compiler_bytes);
        if(complete)break;
    }
    CHECK(complete && sites);
    printf("Array all actual runtime faults: which=%u count=%lld sites=%zu physical=0/0\n",which,(long long)count,sites);
}
static void array_default_values(XrXirProgram *program,uint32_t entry) {
    const int64_t counts[]={3,0,-1,INT64_MIN,INT64_MAX,4};
    for(size_t i=0;i<6;++i) {
        XrXirInstance *instance=array_instance(program);CHECK(instance);XrXirValue arg={XR_XIR_I64,0,counts[i]};
        XrXirCallResult result=array_drive(instance,entry,&arg);
        if(counts[i]<0)CHECK(result.status==XR_XIR_CALL_NUMERIC_RANGE && result.panic.detail.code==422);
        else if(counts[i]==INT64_MAX)CHECK(result.status==XR_XIR_CALL_LIMIT);
        else {
            CHECK(result.status==XR_XIR_CALL_RETURNED);XrXirValue value={0};
            CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
            CHECK(value.type==XR_XIR_I64 && value.payload==counts[i]);xr_xir_value_drop(&value);
        }
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);CHECK(!runtime_live && !runtime_bytes);
    }
}
static void array_growth_transaction(XrXirProgram *program,uint32_t entry) {
    bool complete=false;
    for(size_t ordinal=0;ordinal<127;++ordinal) {
        XrXirValue value=array_success(program,entry,NULL),snapshot={0};
        CHECK(xr_xir_value_copy(&value,&snapshot)==XR_XIR_VALUE_OK);
        XrXirValueAdmission admission=array_admission(&value);XrXirValuePlace place={value.type,&value.payload};
        XrXirValue appended={XR_XIR_I64,0,99};int64_t original=value.payload;
        runtime_attempts=0;runtime_fail_at=ordinal;
        XrXirValueStatus status=xr_xir_array_push(&place,&appended,&admission);
        size_t attempts=runtime_attempts;runtime_fail_at=SIZE_MAX;
        if(attempts>ordinal) {
            CHECK(status==XR_XIR_VALUE_OOM && value.payload==original);
            array_contents(&value,ARRAY_INTEGERS);
        } else {
            CHECK(status==XR_XIR_VALUE_OK && array_length(&value)==3 && value.payload!=original);
            XrXirValue last=array_element(&value,2);CHECK(last.type==XR_XIR_I64 && last.payload==99);xr_xir_value_drop(&last);
            complete=true;printf("Array COW growth all %zu actual faults: unchanged precommit value and snapshot\n",ordinal);
        }
        array_contents(&snapshot,ARRAY_INTEGERS);xr_xir_value_drop(&value);xr_xir_value_drop(&snapshot);
        CHECK(!runtime_live && !runtime_bytes);if(complete)break;
    }
    CHECK(complete);
}
static int array_case(XrXirProgram *program,ArrayProgramEntries entries,unsigned mode) {
    CHECK(mode<15 && !runtime_live && !runtime_bytes);
    unsigned which=mode==0?ARRAY_ALIAS_CHANGED:mode<=4?mode-1:mode==9?ARRAY_ALIAS_CHANGED:mode<=9?mode-5:mode==10 || mode==11?ARRAY_DEFAULT:mode==14?ARRAY_APPEND_MANAGED:ARRAY_APPEND;
    uint32_t entry=entries.entries[which];int code=0;
    if(mode==0 || (mode>=1 && mode<=4)) {
        if(!mode) {XrXirValue initial=array_success(program,entries.entries[ARRAY_ALIAS_INITIAL],NULL);array_contents(&initial,ARRAY_ALIAS_INITIAL);xr_xir_value_drop(&initial);}
        if(mode==1) {
            XrXirValue repeated=array_success(program,entries.entries[ARRAY_REPEATED_READ],NULL);
            array_contents(&repeated,ARRAY_STRINGS);xr_xir_value_drop(&repeated);
        }
        XrXirValue value=array_success(program,entry,NULL);array_contents(&value,which);
        code=mode? (int)array_length(&value):0;
        /* The result owns its arena and backing after execution has gone. */
        xr_xir_compile_program_drop(program);program=NULL;array_contents(&value,which);xr_xir_value_drop(&value);
    } else if(mode<=9)array_fault_scan(program,entry,which,0);
    else if(mode==10)array_default_values(program,entry);
    else if(mode==11)for(int64_t count=3;count>=-1;count=count==3?0:-1) {array_fault_scan(program,entry,which,count);if(count<0)break;}
    else if(mode==12) {
        XrXirValue arg={XR_XIR_I64,0,42};
        for(unsigned i=0;i<2;++i) {XrXirValue value=array_success(program,entry,&arg);CHECK(value.type==XR_XIR_I64 && value.payload==9);xr_xir_value_drop(&value);}
    } else array_fault_scan(program,entry,which,42);
    if(mode==9)array_growth_transaction(program,entries.entries[ARRAY_INTEGERS]);
    if(mode==14) {
        XrXirValue retained=array_success(program,entries.entries[ARRAY_MANAGED_VALUES],NULL);
        xr_xir_compile_program_drop(program);program=NULL;
        CHECK(array_length(&retained)==9);
        for(int64_t i=0;i<9;++i) {
            XrXirValue element=array_element(&retained,i);array_string(&element,"a\0b",3);xr_xir_value_drop(&element);
        }
        xr_xir_value_drop(&retained);
    }
    if(program)xr_xir_compile_program_drop(program);
    CHECK(!runtime_live && !runtime_bytes);return code;
}
#endif // XIR_ARRAY_GENERATED_CASES_H
