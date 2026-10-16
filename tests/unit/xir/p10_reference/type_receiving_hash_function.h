/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * type_receiving_hash_function.h - Hash callbacks keep exact birth authority
 */
#ifndef TYPE_RECEIVING_HASH_FUNCTION_H
#define TYPE_RECEIVING_HASH_FUNCTION_H
static void received_hash_functions(SourceFixtureOwner *compiler) {
    XrXirTypeNode callback={.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_NONE};
    XrXirTypeNode original[]={callback,{.kind=XR_XIR_TYPE_MAP,.element=XR_XIR_I64,.result=type(0)}};
    XrXirTypeNode shifted[]={{.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_BOOL},callback,
        {.kind=XR_XIR_TYPE_MAP,.element=XR_XIR_I64,.result=type(1)}};
    XrXirTypes pools[]={{original,2,NULL,NULL},{shifted,3,NULL,NULL}};
    XrXirTypeArena *birth=NULL,*target=NULL;
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&pools[0],&birth)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&pools[1],&target)==XR_XIR_VALUE_OK);
    XirTypeReceivingRoot root={type(1),type(2)};
    CHECK(xir_compile_type_arena_receive(&compiler->context,target,birth,&root,1)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1024*1024,&domain)==XR_XIR_VALUE_OK);
    uint64_t physical=xr_xir_domain_stats(domain).live_bytes;
    XrXirValueAdmission admission={birth,domain,received_function_admit,NULL,4000000,128*1024};
    ReceivedFunctionContext context={.admission=&admission,.birth=birth};admission.context=&context;
    XrXirFunctionBinding binding={&context,received_function_release,0,NULL,0};
    XrXirValue value={0},map={0},alias={0},read={0},key={XR_XIR_I64,0,7};
    CHECK(xr_xir_function_new(domain,birth,type(0),&binding,&admission,&value)==XR_XIR_VALUE_OK);
    context.value=&value;
    CHECK(xr_xir_map_new(type(1),&key,&value,1,&admission,&map)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&map,&alias)==XR_XIR_VALUE_OK);
    admission.arena=target;bool found=false;unsigned calls=context.calls;
    CHECK(xr_xir_map_get(&map,&key,&admission,&read,&found)==XR_XIR_VALUE_OK && found);
    CHECK(read.type==value.type && read.payload==value.payload && context.calls>calls && admission.arena==target);
    xr_xir_value_drop(&read);context.reject=true;found=false;calls=context.calls;
    uint64_t work=admission.work,scratch=admission.scratch_bytes;int64_t payload=map.payload;
    CHECK(xr_xir_map_get(&map,&key,&admission,&read,&found)==XR_XIR_VALUE_BAD_ARGUMENT && !found && !read.type);
    CHECK(context.calls==calls+1 && admission.arena==target && admission.work<work && admission.scratch_bytes==scratch);
    XrXirValuePlace place={type(2),&map.payload};calls=context.calls;
    CHECK(xr_xir_map_set(&place,&key,&value,&admission)==XR_XIR_VALUE_BAD_ARGUMENT && map.payload==payload &&
        alias.payload==payload && context.calls==calls+1 && admission.arena==target);
    context.reject=false;
    CHECK(xr_xir_map_set(&place,&key,&value,&admission)==XR_XIR_VALUE_OK && map.payload!=payload);
    CHECK(xr_xir_value_arena(&map)==birth && admission.arena==target && admission.scratch_bytes==scratch);
    xr_xir_value_drop(&map);xr_xir_value_drop(&alias);xr_xir_value_drop(&value);
    CHECK(context.releases==1 && xr_xir_domain_stats(domain).live_bytes==physical);
    xr_xir_compile_type_arena_drop(target);xr_xir_compile_type_arena_drop(birth);xr_xir_domain_drop(domain);
}
#endif // TYPE_RECEIVING_HASH_FUNCTION_H
