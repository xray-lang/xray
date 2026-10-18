/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_derived_output_policy.h - Complete former size refusals and caller bounds
 */
#ifndef XIR_DERIVED_OUTPUT_POLICY_H
#define XIR_DERIVED_OUTPUT_POLICY_H
#include "base/xsha256.h"
#include "xir/xxir_ctfe.h"
#include "xir/xxir_array.h"
#include "xir/xxir_tuple.h"
typedef XrXirStatus (*DerivedPolicySource)(const XrXirCompileContext *,const char *,
    XrXirSourceResult *,XrXirSourceDiagnostic *);
static void derived_policy_value(const XrXirValue *value,bool pair) {
    const XrXirTypeArena *arena=xr_xir_value_arena(value);CHECK(arena);
    if(pair) {
        const XrXirTypeNode *tuple=xr_xir_tuple_signature(xr_xir_compile_type_arena_types(arena),(XrXirType)value->type);
        CHECK(tuple && tuple->parameter_count==2);
        for(uint32_t i=0;i<2;++i) {
            XrXirValue item={0};CHECK(tuple->parameters[i].type==XR_XIR_I64);
            CHECK(xr_xir_tuple_get(value,i,&item)==XR_XIR_VALUE_OK);
            CHECK(item.type==XR_XIR_I64 && !item.reserved && item.payload==(int64_t)i+2);xr_xir_value_drop(&item);
        }
    }else {
        XrXirValueAdmission admission={.arena=arena,.work=UINT64_MAX,.scratch_bytes=65536};int64_t length=0;
        CHECK(xr_xir_array_len(value,&admission,&length)==XR_XIR_VALUE_OK && length==9);
        for(int64_t i=0;i<length;++i) {
            XrXirValue item={0};XrXirFaultDetail fault={0};
            CHECK(xr_xir_array_get(value,i,&admission,&item,&fault)==XR_XIR_VALUE_OK);
            CHECK(item.type==XR_XIR_I8 && !item.reserved && item.payload==i+1);xr_xir_value_drop(&item);
        }
    }
}
static void derived_policy_receiving(DerivedPolicySource check,const XrCompileResourceLimits *limits,bool pair) {
    const char *text=pair?
        "fn large()->(i64,i64){return (2,3)}\nexport fn table()->(i64,i64){return comptime large()}\n":
        "fn large()->Array<i8>{return [1,2,3,4,5,6,7,8,9]}\nexport fn table()->Array<i8>{return comptime large()}\n";
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(check(&context,text,&source,&diagnostic)==XR_XIR_OK && source.checked && source.snapshot);
    const XrXirModule *module=xr_xir_compile_artifact_module(source.checked);uint32_t entry=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];
        if(body->name_length!=5 || memcmp(body->name,"table",5))continue;
        CHECK(entry==UINT32_MAX);entry=f;
        for(uint32_t i=0;i<body->instruction_count;++i)
            CHECK(body->instructions[i].op!=XR_XIR_CALL && body->instructions[i].op!=XR_XIR_CALL_INDIRECT && body->instructions[i].op!=XR_XIR_FUNCTION_REF);
    }
    CHECK(entry!=UINT32_MAX);
    XrXirCheckedPacket packet={0};XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    CHECK(xr_xir_compile_checked_write(closed,&packet,NULL)==XR_XIR_OK);uint8_t identity[32]={0};
    CHECK(xr_compile_resources_work(context.resources,packet.length)==XR_COMPILE_RESOURCE_OK);
    xr_sha256(packet.bytes,packet.length,identity);xr_xir_compile_checked_packet_free(&packet);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    module=xr_xir_compile_artifact_module(lowered);entry=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==5 && !memcmp(module->functions[f].name,"table",5)) { CHECK(entry==UINT32_MAX);entry=f; }
    CHECK(entry!=UINT32_MAX);
    XrXirCtfeCallRequest request={.evaluation={.artifact=lowered,.function=entry,.target=target,
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,8}},.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
    memcpy(request.evaluation.checked_identity,identity,32);XrXirValue value={0};
    CHECK(xr_xir_compile_ctfe_calls(&request,&value)==XR_XIR_CTFE_OUTPUT_LIMIT && !value.type && !value.reserved && !value.payload);
    request.evaluation.limits.output_bytes=pair?15:8;
    CHECK(xr_xir_compile_ctfe_calls(&request,&value)==XR_XIR_CTFE_OUTPUT_LIMIT && !value.type && !value.reserved && !value.payload);
    request.evaluation.limits.output_bytes=pair?16:9;
    CHECK(xr_xir_compile_ctfe_calls(&request,&value)==XR_XIR_CTFE_OK);derived_policy_value(&value,pair);xr_xir_value_drop(&value);
    request.evaluation.limits.output_bytes=XR_XIR_CTFE_MAX_OUTPUT_BYTES+1;
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_xir_compile_ctfe_calls(&request,&value)==XR_XIR_CTFE_BAD_ARGUMENT && !value.type && !value.reserved && !value.payload);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && !memcmp(&before,&after,sizeof(before)));
    request.evaluation.limits.output_bytes=XR_XIR_CTFE_MAX_OUTPUT_BYTES;
    CHECK(XR_XIR_CTFE_OUTPUT_POLICY_VERSION==2 && request.evaluation.limits.output_bytes==UINT64_C(8388608));
    CHECK(xr_xir_compile_ctfe_calls(&request,&value)==XR_XIR_CTFE_OK);
    xr_xir_compile_artifact_free(lowered);xr_compile_resources_release(context.resources);
    derived_policy_value(&value,pair);xr_xir_value_drop(&value);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
}
#endif // XIR_DERIVED_OUTPUT_POLICY_H
