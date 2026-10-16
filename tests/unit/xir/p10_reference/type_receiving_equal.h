/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * type_receiving_equal.h - Equality across actual boxed and compact owners
 */
#ifndef TYPE_RECEIVING_EQUAL_H
#define TYPE_RECEIVING_EQUAL_H
#include "xir/xxir_equal.h"
#include "xir/xxir_nullable.h"
static XrXirTypeArena *equal_receiving_arena(SourceFixtureOwner *compiler,unsigned shift) {
    XrXirTypeNode nodes[4]={0};
    if (shift) nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_BOOL};
    nodes[shift]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    nodes[shift+1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=type(shift)};
    nodes[shift+2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=type(shift+1)};
    XrXirTypes types={nodes,shift+3,NULL,NULL};XrXirTypeArena *arena=NULL;
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&types,&arena)==XR_XIR_VALUE_OK);return arena;
}
static void received_value_equality(SourceFixtureOwner *compiler) {
    XrXirTypeArena *source=equal_receiving_arena(compiler,0),*target=equal_receiving_arena(compiler,1);
    XrXirTypeArena *unrelated=equal_receiving_arena(compiler,1);
    XirTypeReceivingRoot root={type(2),type(3)};
    CHECK(xir_compile_type_arena_receive(&compiler->context,target,source,&root,1)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1024*1024,&domain)==XR_XIR_VALUE_OK);
    uint64_t empty=xr_xir_domain_stats(domain).live_bytes;
    XrXirValueAdmission admission={source,domain,NULL,NULL,UINT64_C(4000000),128*1024};
    XrXirValue scalar={XR_XIR_I64,0,41},inner={0},some={0},none={0},left={0},copy={0};
    CHECK(xr_xir_array_new(type(0),&scalar,1,&admission,&inner)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new(type(1),&inner,&admission,&some)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new(type(1),NULL,&admission,&none)==XR_XIR_VALUE_OK);
    XrXirValue values[]={some,none};
    CHECK(xr_xir_array_new(type(2),values,2,&admission,&left)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&left,&copy)==XR_XIR_VALUE_OK);
    admission.arena=target;
    XrXirValue incoming={0},present={0},absent={0},right={0};
    CHECK(xr_xir_array_new(type(1),&scalar,1,&admission,&incoming)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new(type(2),&incoming,&admission,&present)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new(type(2),NULL,&admission,&absent)==XR_XIR_VALUE_OK);
    XrXirValue received[]={present,absent};
    CHECK(xr_xir_array_new(type(3),received,2,&admission,&right)==XR_XIR_VALUE_OK);
    bool result=false;
    CHECK(xr_xir_value_equal(&left,&right,type(3),&admission,&result)==XR_XIR_VALUE_OK && result);
    CHECK(xr_xir_value_equal(&some,&present,type(2),&admission,&result)==XR_XIR_VALUE_OK && result);
    XrXirValue changed_inner={0},changed_some={0},changed={0};scalar.payload=42;
    CHECK(xr_xir_array_new(type(1),&scalar,1,&admission,&changed_inner)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new(type(2),&changed_inner,&admission,&changed_some)==XR_XIR_VALUE_OK);
    received[0]=changed_some;
    CHECK(xr_xir_array_new(type(3),received,2,&admission,&changed)==XR_XIR_VALUE_OK);
    XrXirValuePlace place={type(3),&left.payload};XrXirFaultDetail fault={0};
    CHECK(xr_xir_array_set(&place,0,&changed_some,&admission,&fault)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_equal(&left,&changed,type(3),&admission,&result)==XR_XIR_VALUE_OK && result);
    CHECK(xr_xir_value_equal(&copy,&changed,type(3),&admission,&result)==XR_XIR_VALUE_OK && !result);
    CHECK(xr_xir_value_equal(&copy,&right,type(3),&admission,&result)==XR_XIR_VALUE_OK && result);
    CHECK(left.type==(uint32_t)type(2) && xr_xir_value_arena(&left)==source &&
        right.type==(uint32_t)type(3) && xr_xir_value_arena(&right)==target);
    admission.arena=unrelated;result=true;
    CHECK(xr_xir_value_equal(&left,&right,type(3),&admission,&result)==XR_XIR_VALUE_BAD_ARGUMENT && result);
    admission.arena=target;admission.work=0;
    CHECK(xr_xir_value_equal(&left,&right,type(3),&admission,&result)==XR_XIR_VALUE_LIMIT && result);
    xr_xir_value_drop(&changed);xr_xir_value_drop(&changed_some);xr_xir_value_drop(&changed_inner);
    xr_xir_value_drop(&right);xr_xir_value_drop(&absent);xr_xir_value_drop(&present);xr_xir_value_drop(&incoming);
    xr_xir_value_drop(&copy);xr_xir_value_drop(&left);xr_xir_value_drop(&none);xr_xir_value_drop(&some);xr_xir_value_drop(&inner);
    CHECK(xr_xir_domain_stats(domain).live_bytes==empty);xr_xir_domain_drop(domain);
    xr_xir_compile_type_arena_drop(unrelated);xr_xir_compile_type_arena_drop(target);xr_xir_compile_type_arena_drop(source);
}
#endif // TYPE_RECEIVING_EQUAL_H
