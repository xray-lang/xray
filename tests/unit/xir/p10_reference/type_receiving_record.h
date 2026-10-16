/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * type_receiving_record.h - Received records retain real field owners and callback authority
 */
#ifndef TYPE_RECEIVING_RECORD_H
#define TYPE_RECEIVING_RECORD_H
#include "xir/xxir_record.h"
#include "xir/xxir_tuple.h"
static void received_record_storage(SourceFixtureOwner *compiler) {
    XrXirLiteral names[]={{"apply",5},{"items",5},{"unit",4}};
    XrXirCallableParameter old_fields[]={{type(0),0},{type(1),0},{XR_XIR_UNIT,0}};
    XrXirCallableParameter new_fields[]={{type(2),0},{type(0),0},{XR_XIR_UNIT,0}};
    XrXirTypeNode callback={.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_NONE};
    XrXirTypeNode old_types[]={callback,{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_RECORD,.parameters=old_fields,.parameter_count=3,.record={names}},
        {.kind=XR_XIR_TYPE_CELL,.element=type(2)},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=old_fields,.parameter_count=3}};
    XrXirTypeNode new_types[]={{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_BOOL},callback,
        {.kind=XR_XIR_TYPE_RECORD,.parameters=new_fields,.parameter_count=3,.record={names}},
        {.kind=XR_XIR_TYPE_CELL,.element=type(3)},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=new_fields,.parameter_count=3}};
    XrXirTypes types[]={{old_types,5,NULL,NULL},{new_types,6,NULL,NULL}};
    XrXirTypeArena *birth=NULL,*target=NULL,*unrelated=NULL;
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&types[0],&birth)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&types[1],&target)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&types[1],&unrelated)==XR_XIR_VALUE_OK);
    XirTypeReceivingRoot roots[]={{type(3),type(4)},{type(4),type(5)}};
    CHECK(xir_compile_type_arena_receive(&compiler->context,target,birth,roots,2)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    uint64_t physical=xr_xir_domain_stats(domain).live_bytes;
    XrXirValueAdmission admission={birth,domain,received_function_admit,NULL,4000000,128*1024};
    ReceivedFunctionContext context={.admission=&admission,.birth=birth};admission.context=&context;
    XrXirFunctionBinding binding={&context,received_function_release,0,NULL,0};
    XrXirValue fn={0},array={0},record={0},snapshot={0},incoming={0},read={0},fresh={0};
    CHECK(xr_xir_function_new(domain,birth,type(0),&binding,&admission,&fn)==XR_XIR_VALUE_OK);
    context.value=&fn;XrXirValue number={XR_XIR_I64,0,42};
    CHECK(xr_xir_array_new(type(1),&number,1,&admission,&array)==XR_XIR_VALUE_OK);
    XrXirValue fields[]={fn,array,{0}};
    CHECK(xr_xir_record_new(type(2),fields,3,&admission,&record)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&record,&snapshot)==XR_XIR_VALUE_OK);
    admission.arena=target;unsigned calls=context.calls;
    CHECK(xr_xir_record_get(&record,0,&admission,&read)==XR_XIR_VALUE_OK);
    CHECK(read.type==fn.type && read.payload==fn.payload && context.calls>calls && admission.arena==target);
    xr_xir_value_drop(&read);
    CHECK(xr_xir_record_new(type(3),fields,3,&admission,&fresh)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_arena(&fresh)==target && fresh.type==(uint32_t)type(3));
    CHECK(xr_xir_value_admit(&fresh,type(3),&admission)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_record_get(&fresh,1,&admission,&read)==XR_XIR_VALUE_OK && read.payload==array.payload && xr_xir_value_arena(&read)==birth);
    xr_xir_value_drop(&read);xr_xir_value_drop(&fresh);
    XrXirValue tuple={0};
    CHECK(xr_xir_tuple_new(type(5),fields,3,&admission,&tuple)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_admit(&tuple,type(5),&admission)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_tuple_get(&tuple,1,&read)==XR_XIR_VALUE_OK && read.payload==array.payload && xr_xir_value_arena(&read)==birth);
    xr_xir_value_drop(&read);xr_xir_value_drop(&tuple);
    number.payload=63;CHECK(xr_xir_array_new(type(0),&number,1,&admission,&incoming)==XR_XIR_VALUE_OK);
    XrXirValuePlace place={type(3),&record.payload};uint64_t scratch=admission.scratch_bytes;
    context.reject=true;int64_t payload=record.payload;calls=context.calls;uint64_t work=admission.work;
    CHECK(xr_xir_record_get(&record,0,&admission,&read)==XR_XIR_VALUE_BAD_ARGUMENT && !read.type);
    CHECK(context.calls==calls+1 && admission.arena==target && admission.work<work);
    calls=context.calls;
    CHECK(xr_xir_record_set(&place,1,&incoming,&admission)==XR_XIR_VALUE_BAD_ARGUMENT && record.payload==payload);
    CHECK(context.calls==calls+1 && snapshot.payload==payload && admission.arena==target);
    context.reject=false;
    CHECK(xr_xir_record_set(&place,1,&incoming,&admission)==XR_XIR_VALUE_OK && record.payload!=snapshot.payload);
    CHECK(xr_xir_value_arena(&record)==birth && record.type==(uint32_t)type(2));
    CHECK(xr_xir_record_get(&record,1,&admission,&read)==XR_XIR_VALUE_OK && read.payload==incoming.payload && xr_xir_value_arena(&read)==target);
    xr_xir_value_drop(&read);
    CHECK(xr_xir_record_get(&snapshot,1,&admission,&read)==XR_XIR_VALUE_OK && read.payload==array.payload && xr_xir_value_arena(&read)==birth);
    xr_xir_value_drop(&read);payload=record.payload;work=admission.work;admission.work=0;
    CHECK(xr_xir_record_set(&place,1,&array,&admission)==XR_XIR_VALUE_LIMIT && record.payload==payload);
    CHECK(admission.arena==target && admission.scratch_bytes==scratch);admission.work=work;
    admission.arena=unrelated;
    CHECK(xr_xir_record_get(&record,1,&admission,&read)==XR_XIR_VALUE_BAD_ARGUMENT && !read.type);
    CHECK(xr_xir_record_set(&place,1,&array,&admission)==XR_XIR_VALUE_BAD_ARGUMENT && record.payload==payload);
    CHECK(admission.arena==unrelated && admission.scratch_bytes==scratch);admission.arena=target;
    xr_xir_value_drop(&record);xr_xir_value_drop(&snapshot);xr_xir_value_drop(&incoming);
    xr_xir_value_drop(&array);xr_xir_value_drop(&fn);
    CHECK(context.releases==1 && xr_xir_domain_stats(domain).live_bytes==physical);
    xr_xir_compile_type_arena_drop(unrelated);xr_xir_compile_type_arena_drop(target);
    xr_xir_compile_type_arena_drop(birth);xr_xir_domain_drop(domain);
    puts("Record/Tuple received boxed children retained actual arenas in both directions; COW, authentic callback reentry/rejection, lookalike rejection, work/scratch restoration and physical baseline passed");
}
#endif // TYPE_RECEIVING_RECORD_H
