/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * typed_display_receiving_cases.h - Real relocated children through execution
 *
 * KEY CONCEPT:
 *   An admitted Array element keeps its original carrier and type owner. The
 *   receiving Program proves the relation before any instance is created.
 */
#ifndef TYPED_DISPLAY_RECEIVING_CASES_H
#define TYPED_DISPLAY_RECEIVING_CASES_H
#include "xir/xxir_program_internal.h"
#include "xir/xxir_type_arena_internal.h"
#include "xir/xxir_array.h"
static void received_program(XrXirCompileContext *context,XrXirProgram *program,uint32_t function) {
    CHECK(function<program->entry_count && program->entries[function].parameter_count==1);
    XrXirType outer=program->entries[function].parameters[0];
    const XrXirTypeNode *node=xr_xir_type_node(program->types,outer);CHECK(node && node->kind==XR_XIR_TYPE_ARRAY);
    XrXirType inner=node->element;node=xr_xir_type_node(program->types,inner);CHECK(node && node->kind==XR_XIR_TYPE_ARRAY && node->element==XR_XIR_I64);
    XrXirTypeNode nodes[3]={
        {.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_BOOL},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64}};
    if(inner==(XrXirType)258){nodes[0].kind=XR_XIR_TYPE_ARRAY;nodes[0].element=XR_XIR_I64;nodes[2].kind=XR_XIR_TYPE_NULLABLE;nodes[2].element=XR_XIR_RUNE;}
    XrXirType source_type=inner==(XrXirType)258?(XrXirType)256:(XrXirType)258;
    XrXirTypes pool={nodes,3,NULL,NULL};XrXirTypeArena *source=NULL;CHECK(xr_xir_compile_type_arena_new(context,&pool,&source)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);uint64_t frame=xr_xir_domain_stats(domain).live_bytes;
    XrXirValueAdmission birth={source,domain,NULL,NULL,100000,1048576},receiver=birth;receiver.arena=program->arena;
    XrXirValue values[]={{XR_XIR_I64,0,7},{XR_XIR_I64,0,8}},array={0},nested={0};
    CHECK(xr_xir_array_new(source_type,values,2,&birth,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new(outer,&array,1,&receiver,&nested)==XR_XIR_VALUE_BAD_ARGUMENT && !nested.type && !nested.payload);
    const XirTypeReceivingRoot root={source_type,inner};
    CHECK(xir_compile_type_arena_receive(context,program->arena,source,&root,1)==XR_XIR_VALUE_OK);
    receiver.work=100000;CHECK(xr_xir_array_new(outer,&array,1,&receiver,&nested)==XR_XIR_VALUE_OK);
    XrXirValue child={0};XrXirFaultDetail fault={0};CHECK(xr_xir_array_get(&nested,0,&receiver,&child,&fault)==XR_XIR_VALUE_OK);
    CHECK(child.type==(uint32_t)source_type && source_type!=inner && xr_xir_value_arena(&child)==source);xr_xir_value_drop(&child);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,function,&nested,1)==XR_XIR_CALL_READY);XrXirCallResult result={0};
    for(unsigned i=0;;++i){CHECK(i<10000);result=xr_xir_instance_poll_bounded(instance,1024).outcome;if(result.status!=XR_XIR_CALL_READY)break;}
    CHECK(result.status==XR_XIR_CALL_RETURNED);XrXirValue text={0};CHECK(xr_xir_instance_take_result(instance,&text)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_value_drop(&array);xr_xir_value_drop(&nested);
    xr_xir_compile_type_arena_drop(source);CHECK(xr_xir_domain_stats(domain).live_bytes==frame);xr_xir_domain_drop(domain);
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&text,&bytes,&length) && length==6 && !memcmp(bytes,"[7, 8]",6));xr_xir_value_drop(&text);
}
#endif // TYPED_DISPLAY_RECEIVING_CASES_H
