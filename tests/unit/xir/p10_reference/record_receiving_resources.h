/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * record_receiving_resources.h - Actual received child allocation and admission bounds
 */
#ifndef RECORD_RECEIVING_RESOURCES_H
#define RECORD_RECEIVING_RESOURCES_H
#include "xir/xxir_type_arena_internal.h"
static void record_received_array(const XrXirValue *record,const XrXirValue *expected,
    int64_t number,XrXirValueAdmission *admission) {
    XrXirValue array={0},read={0};XrXirFaultDetail fault={0};
    CHECK(xr_xir_record_get(record,0,admission,&array)==XR_XIR_VALUE_OK);
    CHECK(array.type==expected->type && array.payload==expected->payload &&
        xr_xir_value_arena(&array)==xr_xir_value_arena(expected));
    CHECK(xr_xir_array_get(&array,0,admission,&read,&fault)==XR_XIR_VALUE_OK &&
        read.type==XR_XIR_I64 && read.payload==number);
    xr_xir_value_drop(&read);xr_xir_value_drop(&array);
}
static void record_receiving_resources(void) {
    XrCompileResourceLimits limits={8388608,4194304,128000000};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    XrXirLiteral name={"items",5};
    XrXirCallableParameter before={(XrXirType)256,0},after={(XrXirType)257,0};
    XrXirTypeNode original[]={{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_RECORD,.parameters=&before,.parameter_count=1,.record={&name}}};
    XrXirTypeNode shifted[]={{.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_BOOL},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_RECORD,.parameters=&after,.parameter_count=1,.record={&name}}};
    XrXirTypes pools[]={{original,2,NULL,NULL},{shifted,3,NULL,NULL}};
    XrXirTypeArena *birth=NULL,*target=NULL;
    CHECK(xr_xir_compile_type_arena_new(&context,&pools[0],&birth)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_type_arena_new(&context,&pools[1],&target)==XR_XIR_VALUE_OK);
    XirTypeReceivingRoot receiving={(XrXirType)257,(XrXirType)258};
    CHECK(xir_compile_type_arena_receive(&context,target,birth,&receiving,1)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission=admit_new(birth,domain);
    XrXirValue number={XR_XIR_I64,0,42},old={0},incoming={0},record={0},snapshot={0},read={0};
    CHECK(xr_xir_array_new((XrXirType)256,&number,1,&admission,&old)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_record_new((XrXirType)257,&old,1,&admission,&record)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&record,&snapshot)==XR_XIR_VALUE_OK);admission.arena=target;number.payload=63;
    CHECK(xr_xir_array_new((XrXirType)257,&number,1,&admission,&incoming)==XR_XIR_VALUE_OK);
    size_t live=runtime_live,bytes=runtime_bytes,sites=0;uint64_t write_work=0;
    for(size_t pass=0;pass<=sites;++pass) {
        XrXirValue current={0};CHECK(xr_xir_value_copy(&record,&current)==XR_XIR_VALUE_OK);
        XrXirValuePlace place={(XrXirType)258,&current.payload};
        XrXirValueAdmission attempt=admit_new(target,domain);runtime_attempts=0;runtime_fail_at=pass?pass-1:SIZE_MAX;
        XrXirValueStatus status=xr_xir_record_set(&place,0,&incoming,&attempt);
        size_t calls=runtime_attempts;runtime_fail_at=SIZE_MAX;
        if(!pass) {CHECK(status==XR_XIR_VALUE_OK);sites=calls;CHECK(sites);write_work=1000000-attempt.work;}
        else CHECK(status==XR_XIR_VALUE_OOM && calls>=pass && current.payload==record.payload);
        CHECK(attempt.arena==target && attempt.scratch_bytes==262144);
        record_received_array(&current,pass?&old:&incoming,pass?42:63,&admission);
        xr_xir_value_drop(&current);CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    XrXirValuePlace place={(XrXirType)258,&record.payload};
    for(uint64_t prefix=0;prefix<write_work;++prefix) {
        XrXirValueAdmission attempt=admit_new(target,domain);attempt.work=prefix;
        CHECK(xr_xir_record_set(&place,0,&incoming,&attempt)==XR_XIR_VALUE_LIMIT && record.payload==snapshot.payload);
        CHECK(attempt.arena==target && attempt.scratch_bytes==262144 && runtime_live==live && runtime_bytes==bytes);
    }
    XrXirValueAdmission exact=admit_new(target,domain);exact.work=write_work;
    CHECK(xr_xir_record_set(&place,0,&incoming,&exact)==XR_XIR_VALUE_OK && !exact.work);
    record_received_array(&record,&incoming,63,&admission);record_received_array(&snapshot,&old,42,&admission);
    live=runtime_live;bytes=runtime_bytes;XrXirValueAdmission measure=admit_new(target,domain);runtime_attempts=0;
    CHECK(xr_xir_record_get(&record,0,&measure,&read)==XR_XIR_VALUE_OK && !runtime_attempts);
    uint64_t read_work=1000000-measure.work;xr_xir_value_drop(&read);int64_t payload=record.payload;
    for(uint64_t prefix=0;prefix<read_work;++prefix) {
        XrXirValueAdmission attempt=admit_new(target,domain);attempt.work=prefix;
        CHECK(xr_xir_record_get(&record,0,&attempt,&read)==XR_XIR_VALUE_LIMIT && !read.type && record.payload==payload);
        CHECK(attempt.arena==target && attempt.scratch_bytes==262144 && runtime_live==live && runtime_bytes==bytes);
    }
    exact=admit_new(target,domain);exact.work=read_work;
    CHECK(xr_xir_record_get(&record,0,&exact,&read)==XR_XIR_VALUE_OK && !exact.work && read.payload==incoming.payload);
    xr_xir_value_drop(&read);
    for(unsigned shortfall=0;shortfall<2;++shortfall) {
        XrXirValueAdmission attempt=admit_new(target,domain);attempt.scratch_bytes=40-shortfall;
        CHECK(xr_xir_record_get(&record,0,&attempt,&read)==(shortfall?XR_XIR_VALUE_LIMIT:XR_XIR_VALUE_OK));
        CHECK(shortfall?!read.type:read.payload==incoming.payload);xr_xir_value_drop(&read);
        CHECK(attempt.scratch_bytes==40-shortfall && attempt.arena==target && record.payload==payload);
    }
    for(unsigned shortfall=0;shortfall<2;++shortfall) {
        XrXirValue current={0};CHECK(xr_xir_value_copy(&record,&current)==XR_XIR_VALUE_OK);
        XrXirValuePlace copy_place={(XrXirType)258,&current.payload};
        XrXirValueAdmission attempt=admit_new(target,domain);attempt.scratch_bytes=40-shortfall;
        CHECK(xr_xir_record_set(&copy_place,0,&incoming,&attempt)==(shortfall?XR_XIR_VALUE_LIMIT:XR_XIR_VALUE_OK));
        CHECK(shortfall?current.payload==record.payload:current.payload!=record.payload);
        CHECK(attempt.scratch_bytes==40-shortfall && attempt.arena==target);
        xr_xir_value_drop(&current);CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    xr_xir_compile_type_arena_drop(target);xr_xir_compile_type_arena_drop(birth);
    record_received_array(&record,&incoming,63,&admission);record_received_array(&snapshot,&old,42,&admission);
    xr_xir_value_drop(&record);xr_xir_value_drop(&snapshot);xr_xir_value_drop(&old);xr_xir_value_drop(&incoming);
    xr_xir_domain_drop(domain);xr_compile_resources_release(context.resources);CHECK(!runtime_live&&!runtime_bytes);
    printf("Received Record<Array>: all%zu actual allocation failures,write work0..%llu exact%llu,read work0..%llu exact%llu,scratch40/39,producer death and physical0/0 PASS\n",
        sites,(unsigned long long)(write_work-1),(unsigned long long)write_work,
        (unsigned long long)(read_work-1),(unsigned long long)read_work);
}
#endif // RECORD_RECEIVING_RESOURCES_H
