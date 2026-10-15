/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * type_receiving_nested.h - Actual mixed-arena leaves through compact paths
 */
#ifndef TYPE_RECEIVING_NESTED_H
#define TYPE_RECEIVING_NESTED_H
static void received_nested_arrays(SourceFixtureOwner *compiler) {
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_ARRAY,.element=type(0)}};
    XrXirTypeNode shifted[]={{.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_BOOL},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},{.kind=XR_XIR_TYPE_ARRAY,.element=type(1)}};
    XrXirTypes pools[]={{nodes,2,NULL,NULL},{shifted,3,NULL,NULL}};
    XrXirTypeArena *source=NULL,*target=NULL;
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&pools[0],&source)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&pools[1],&target)==XR_XIR_VALUE_OK);
    XirTypeReceivingRoot root={type(1),type(2)};
    CHECK(xir_compile_type_arena_receive(&compiler->context,target,source,&root,1)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1024*1024,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={source,domain,NULL,NULL,UINT64_C(4000000),128*1024};
    int32_t narrow=17;XrXirValuePlace invalid={XR_XIR_I32,&narrow};XrXirValue rejected={XR_XIR_I64,0,19};
    CHECK(xr_xir_array_push(&invalid,&rejected,&admission)==XR_XIR_VALUE_BAD_ARGUMENT && narrow==17);
    XrXirValue scalar={XR_XIR_I64,0,41},inner={0},outer={0},incoming={0},read={0},copy={0};
    CHECK(xr_xir_array_new(type(0),&scalar,1,&admission,&inner)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new(type(1),&inner,1,&admission,&outer)==XR_XIR_VALUE_OK);
    admission.arena=target;scalar.payload=51;
    CHECK(xr_xir_array_new(type(1),&scalar,1,&admission,&incoming)==XR_XIR_VALUE_OK);
    XrXirValuePlace place={type(2),&outer.payload};XrXirFaultDetail fault={0};
    CHECK(xr_xir_array_set(&place,0,&incoming,&admission,&fault)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_get(&outer,0,&admission,&read,&fault)==XR_XIR_VALUE_OK);
    CHECK(read.type==incoming.type && read.payload==incoming.payload && xr_xir_value_arena(&read)==target);
    xr_xir_value_drop(&read);
    CHECK(xr_xir_value_admit(&outer,type(2),&admission)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_reserve(&outer,8,&admission,&copy)==XR_XIR_VALUE_OK);
    const XrXirValuePathStep steps[]={{XR_XIR_PATH_INDEX,type(2),0},{XR_XIR_PATH_INDEX,type(1),0}};
    const XrXirValuePath path={steps,2};scalar.payload=73;
    CHECK(xr_xir_value_path_write(&place,&path,&scalar,&admission,&fault)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_path_read(&place,&path,&admission,&read,&fault)==XR_XIR_VALUE_OK && read.payload==73);
    xr_xir_value_drop(&read);place.payload=&copy.payload;
    CHECK(xr_xir_value_path_read(&place,&path,&admission,&read,&fault)==XR_XIR_VALUE_OK && read.payload==51);
    xr_xir_value_drop(&read);
    CHECK(xr_xir_array_get(&inner,0,&admission,&read,&fault)==XR_XIR_VALUE_OK && read.payload==41);
    xr_xir_value_drop(&read);
    xr_xir_value_drop(&copy);xr_xir_value_drop(&outer);xr_xir_value_drop(&inner);xr_xir_value_drop(&incoming);
    xr_xir_compile_type_arena_drop(target);xr_xir_compile_type_arena_drop(source);xr_xir_domain_drop(domain);
}
#endif // TYPE_RECEIVING_NESTED_H
