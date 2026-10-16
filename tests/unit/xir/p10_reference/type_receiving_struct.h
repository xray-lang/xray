/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * type_receiving_struct.h - Received nested Struct reads retain actual owners
 */
#ifndef TYPE_RECEIVING_STRUCT_H
#define TYPE_RECEIVING_STRUCT_H
#include "xir/xxir_struct.h"
static void received_struct_storage(SourceFixtureOwner *compiler) {
    const XrXirNominalFieldIdentity field={{"value",5},XR_XIR_FIELD_MUTABLE};
    const XrXirNominalIdentity identities[]={
        {{"owner",5},{"Inner",5},1,0,&field,1,XR_XIR_NOMINAL_STRUCT,NULL,0,0,{0}},
        {{"owner",5},{"Outer",5},1,0,&field,1,XR_XIR_NOMINAL_STRUCT,NULL,0,0,{0}}};
    const XrXirNominalTable table={NULL,2,identities};
    const XrXirType scalar=XR_XIR_I64,old_inner=type(0),new_inner=type(1);
    XrXirTypeNode original[]={
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,NULL,0,&scalar,1}},
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={1,NULL,0,&old_inner,1}}};
    XrXirTypeNode shifted[]={
        {.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_BOOL},
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,NULL,0,&scalar,1}},
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={1,NULL,0,&new_inner,1}}};
    XrXirTypes pools[]={{original,2,&table,NULL},{shifted,3,&table,NULL}};
    XrXirTypeArena *source=NULL,*target=NULL,*unrelated=NULL;
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&pools[0],&source)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&pools[1],&target)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&pools[1],&unrelated)==XR_XIR_VALUE_OK);
    XirTypeReceivingRoot root={type(1),type(2)};
    CHECK(xir_compile_type_arena_receive(&compiler->context,target,source,&root,1)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1024*1024,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={source,domain,NULL,NULL,UINT64_C(4000000),128*1024};
    XrXirValue number={XR_XIR_I64,0,41},inner={0},outer={0},read={0},leaf={0};
    CHECK(xr_xir_struct_new(type(0),&number,1,&admission,&inner)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_struct_new(type(1),&inner,1,&admission,&outer)==XR_XIR_VALUE_OK);
    admission.arena=target;
    uint64_t work=admission.work;admission.work=0;
    CHECK(xr_xir_struct_get(&outer,0,&admission,&read)==XR_XIR_VALUE_LIMIT && !read.type);
    admission.work=work;
    CHECK(xr_xir_struct_get(&outer,0,&admission,&read)==XR_XIR_VALUE_OK);
    CHECK(read.type==(uint32_t)type(0) && xr_xir_value_arena(&read)==source && admission.arena==target);
    CHECK(xr_xir_struct_get(&read,0,&admission,&leaf)==XR_XIR_VALUE_OK && leaf.type==XR_XIR_I64 && leaf.payload==41);
    xr_xir_value_drop(&leaf);xr_xir_value_drop(&read);
    CHECK(xr_xir_struct_get(&outer,1,&admission,&read)==XR_XIR_VALUE_BAD_ARGUMENT && !read.type);
    admission.arena=unrelated;
    CHECK(xr_xir_struct_get(&outer,0,&admission,&read)==XR_XIR_VALUE_BAD_ARGUMENT && !read.type);
    xr_xir_value_drop(&outer);xr_xir_value_drop(&inner);
    xr_xir_compile_type_arena_drop(unrelated);xr_xir_compile_type_arena_drop(target);
    xr_xir_compile_type_arena_drop(source);xr_xir_domain_drop(domain);
}
#endif // TYPE_RECEIVING_STRUCT_H
