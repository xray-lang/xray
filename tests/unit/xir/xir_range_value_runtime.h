/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_range_value_runtime.h - Actual class values after all producer and code owners retire
 */
#ifndef XIR_RANGE_VALUE_RUNTIME_H
#define XIR_RANGE_VALUE_RUNTIME_H
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_class.h"
#include "shared/xr_range_core.h"
#define RANGE_SELECTION_COUNT 13u
static XrXirCompileContext range_context(void) {
    XrXirCompileContext context={0};const XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();return context;
}
static XrXirValue range_take(XrXirInstance *instance,uint32_t function,const XrXirValue *args,uint32_t count) {
    CHECK(xr_xir_instance_start(instance,function,args,count)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static void range_fields(const XrXirValue *value,int64_t start,int64_t end,bool inclusive) {
    CHECK(!value->reserved && xr_xir_value_arena(value));
    XrXirValueAdmission admission={.arena=xr_xir_value_arena(value),.work=UINT64_MAX};
    const int64_t expected[]={start,end,1,inclusive?1:0};
    for(uint32_t f=0;f<4;++f) {
        XrXirValue field={0};CHECK(xr_xir_class_get(value,f,&admission,&field)==XR_XIR_VALUE_OK);
        CHECK(field.type==(uint32_t)(f==3?XR_XIR_BOOL:XR_XIR_I64) && !field.reserved && field.payload==expected[f]);
        xr_xir_value_drop(&field);
    }
}
static void range_integer(XrXirValue *value,int64_t expected) {
    CHECK(value->type==XR_XIR_I64 && !value->reserved && value->payload==expected);xr_xir_value_drop(value);
}
static const struct {int64_t start,end;bool closed;int64_t count;} range_examples[]={
        {2,5,false,3},{2,5,true,4},{5,5,false,0},{5,5,true,1},
        {5,2,false,0},{5,2,true,0},{-5,-2,false,3},{-5,-2,true,4},{-2,3,true,6},
        {INT64_MIN,-1,false,INT64_MAX},{INT64_MIN,-2,true,INT64_MAX},
        {INT64_MIN,-1,true,INT64_MAX},{INT64_MIN,INT64_MAX,false,INT64_MAX},
        {INT64_MIN,INT64_MAX,true,INT64_MAX},{INT64_MAX,INT64_MAX,true,1},
        {INT64_MIN,INT64_MIN,true,1},{0,INT64_MAX,false,INT64_MAX},{0,INT64_MAX,true,INT64_MAX}
    };
static void range_index_cases(XrXirInstance *instance,const uint32_t selection[RANGE_SELECTION_COUNT],
    int64_t start,int64_t end,bool closed) {
    const XrRangeCore oracle=xr_range_core_make_with_bound(start,end,1,closed);
    int64_t length=xr_range_core_length(oracle);
    const int64_t keys[]={INT64_MIN,-1,0,1,length?length-1:0,length,INT64_MAX};
    for(size_t k=0;k<sizeof(keys)/sizeof(*keys);++k) {
        bool valid=false;int64_t expected=xr_range_core_index(oracle,keys[k],&valid);
        XrXirValue args[]={{XR_XIR_I64,0,start},{XR_XIR_I64,0,end},{XR_XIR_BOOL,0,closed},{XR_XIR_I64,0,keys[k]}};
        XrXirValue value=range_take(instance,selection[12],args,4);range_integer(&value,valid?expected:430);
        if(valid){value=range_take(instance,selection[9],args,4);range_integer(&value,expected);}
    }
}
static XrXirValue range_runtime_cases(XrXirProgram *program,const uint32_t selection[RANGE_SELECTION_COUNT]) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    for(size_t i=0;i<sizeof(range_examples)/sizeof(*range_examples);++i) {
        XrXirValue args[]={{XR_XIR_I64,0,range_examples[i].start},{XR_XIR_I64,0,range_examples[i].end},{XR_XIR_BOOL,0,range_examples[i].closed}};
        XrXirValue literal=range_take(instance,selection[range_examples[i].closed?1:0],args,2);
        XrXirValue constructed=range_take(instance,selection[2],args,3);
        range_fields(&literal,range_examples[i].start,range_examples[i].end,range_examples[i].closed);
        range_fields(&constructed,range_examples[i].start,range_examples[i].end,range_examples[i].closed);
        XrXirValue count=range_take(instance,selection[3],args,3);range_integer(&count,range_examples[i].count);
        count=range_take(instance,selection[6],&literal,1);range_integer(&count,range_examples[i].count);
        count=range_take(instance,selection[6],&constructed,1);range_integer(&count,range_examples[i].count);
        count=range_take(instance,selection[7],&literal,1);range_integer(&count,range_examples[i].count);
        count=range_take(instance,selection[7],&constructed,1);range_integer(&count,range_examples[i].count);
        XrXirValue endpoints=range_take(instance,selection[4],args,3);
        CHECK(endpoints.type==XR_XIR_BOOL && !endpoints.reserved && endpoints.payload==1);xr_xir_value_drop(&endpoints);
        range_index_cases(instance,selection,range_examples[i].start,range_examples[i].end,range_examples[i].closed);
        xr_xir_value_drop(&literal);xr_xir_value_drop(&constructed);
    }
    XrXirValue once=range_take(instance,selection[5],NULL,0);range_integer(&once,11212);
    once=range_take(instance,selection[10],NULL,0);range_integer(&once,812);
    once=range_take(instance,selection[11],NULL,0);range_integer(&once,8100);
    for(int64_t closed=0;closed<2;++closed) {
        XrXirValue flag={XR_XIR_BOOL,0,closed};once=range_take(instance,selection[8],&flag,1);
        range_integer(&once,closed?601:501);
    }
    XrXirValue args[]={{XR_XIR_I64,0,INT64_MIN},{XR_XIR_I64,0,INT64_MAX},{XR_XIR_BOOL,0,1}};
    XrXirValue value=range_take(instance,selection[2],args,3),kept={0};
    CHECK(xr_xir_value_copy(&value,&kept)==XR_XIR_VALUE_OK);xr_xir_value_drop(&value);
    xr_xir_compile_program_drop(program);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    return kept;
}
static void range_retained(XrXirValue *value) {
    range_fields(value,INT64_MIN,INT64_MAX,true);xr_xir_value_drop(value);
}
#endif // XIR_RANGE_VALUE_RUNTIME_H
