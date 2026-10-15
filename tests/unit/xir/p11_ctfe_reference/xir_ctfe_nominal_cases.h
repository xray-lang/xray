/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ctfe_nominal_cases.h - Independent nominal payloads and owning failure controls
 */
#ifndef XIR_CTFE_NOMINAL_CASES_H
#define XIR_CTFE_NOMINAL_CASES_H
#include "xir/xxir_struct.h"
#include "xir/xxir_enum.h"
static void reference_nominal_value(const XrXirValue *value,const CtfeReferenceCase *selected,XrXirDomain *reader) {
    const XrXirTypeArena *arena=xr_xir_value_arena(value);CHECK(arena && xr_xir_value_valid(value));
    const XrXirTypes *types=xr_xir_compile_type_arena_types(arena);const XrXirTypeNode *node=xr_xir_type_node(types,(XrXirType)value->type);
    CHECK(node && node->kind==XR_XIR_TYPE_NOMINAL);
    XrXirValueAdmission admission={.arena=arena,.domain=reader,.work=UINT64_MAX,.scratch_bytes=65536};XrXirValue record={0};
    bool nested=!strcmp(selected->name,"nominal_nested_export"),empty=strstr(selected->name,"empty") || strstr(selected->name,"single");
    if (xr_xir_type_is_enum(types,(XrXirType)value->type)) {
        uint32_t variant=UINT32_MAX;CHECK(xr_xir_enum_variant(value,&variant)==XR_XIR_VALUE_OK && variant==(empty?0u:1u));
        if (empty) return;
        CHECK(xr_xir_enum_get(value,variant,0,&admission,&record)==XR_XIR_VALUE_OK);
        if (!strcmp(selected->name,"nominal_enum_array_export")) {
            XrXirValue tuple={0},number={0},text={0};XrXirFaultDetail fault={0};
            CHECK(xr_xir_array_get(&record,0,&admission,&tuple,&fault)==XR_XIR_VALUE_OK);
            CHECK(xr_xir_tuple_get(&tuple,0,&number)==XR_XIR_VALUE_OK && number.type==XR_XIR_I8 && number.payload==7);
            CHECK(xr_xir_tuple_get(&tuple,1,&text)==XR_XIR_VALUE_OK);reference_text_value(&text,"ab");
            xr_xir_value_drop(&number);xr_xir_value_drop(&text);xr_xir_value_drop(&tuple);xr_xir_value_drop(&record);return;
        }
        if (!nested) { reference_text_value(&record,"ab");xr_xir_value_drop(&record);return; }
    } else {
        CHECK(xr_xir_type_is_struct(types,(XrXirType)value->type));
        if (empty) { CHECK(!node->nominal.field_count);return; }
        CHECK(xr_xir_value_copy(value,&record)==XR_XIR_VALUE_OK);
    }
    XrXirValue number={0},text={0};
    CHECK(xr_xir_struct_get(&record,0,&admission,&number)==XR_XIR_VALUE_OK && number.type==XR_XIR_I8 && number.payload==7);
    CHECK(xr_xir_struct_get(&record,1,&admission,&text)==XR_XIR_VALUE_OK);reference_text_value(&text,"ab");
    xr_xir_value_drop(&number);xr_xir_value_drop(&text);xr_xir_value_drop(&record);
}
static uint32_t reference_nominal_function(const XrXirModule *module,const CtfeReferenceCase *selected) {
    if (strcmp(selected->name,"lexical_private_direct")) return reference_run_function(module);
    uint32_t found=UINT32_MAX;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];const XrXirFunctionIdentity *identity=&module->declarations->functions[f];
        if (!identity->nominal_owner || identity->method_kind!=XR_XIR_MEMBER_HELPER || body->name_length<9 ||
            memcmp(body->name,"$comptime",9) || !xr_xir_type_is_struct(module->types,body->result)) continue;
        CHECK(found==UINT32_MAX && !body->parameter_count);found=f;
    }
    CHECK(found!=UINT32_MAX);return found;
}
static void reference_nominal(const XrXirCompileContext *context,XrXirSourceResult *source,const CtfeReferenceCase *selected) {
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t function=reference_nominal_function(module,selected);
    const XrXirFunction *body=&module->functions[function];bool recipe=false;
    for (uint32_t i=0;i<body->instruction_count;++i) {
        CHECK(body->instructions[i].op!=XR_XIR_CALL && body->instructions[i].op!=XR_XIR_CALL_INDIRECT);
        if (body->instructions[i].op==XR_XIR_STRUCT_NEW || body->instructions[i].op==XR_XIR_ENUM_NEW) recipe=true;
    }
    CHECK(recipe);XrXirDomain *reader=NULL;CHECK(xr_xir_compile_domain_new(context->resources,UINT64_MAX,&reader)==XR_XIR_VALUE_OK);
    XrXirCtfeRequest request={.artifact=lowered,.function=function,.target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,(uint64_t)selected->value}};
    memcpy(request.checked_identity,identity,sizeof(identity));XrXirValue output={0};XrCompileResourceStats baseline=reference_stats(context);
    size_t counts[2]={0};unsigned modes=strstr(selected->name,"direct")?2:1;
    for (unsigned mode=0;mode<modes;++mode) {
        size_t first=source_fixture_compile_attempts;XrXirCtfeStatus initial=reference_text_evaluate(&request,mode!=0,&output);
        if (initial!=XR_XIR_CTFE_OK) fprintf(stderr,"nominal initial mode=%u status=%d\n",mode,(int)initial);CHECK(initial==XR_XIR_CTFE_OK);
        counts[mode]=source_fixture_compile_attempts-first;CHECK(counts[mode]>0);reference_nominal_value(&output,selected,reader);xr_xir_value_drop(&output);
        for (size_t fault=0;fault<counts[mode];++fault) {
            source_fixture_compile_injected=false;source_fixture_compile_fail_at=source_fixture_compile_attempts+fault;
            CHECK(reference_text_evaluate(&request,mode!=0,&output)==XR_XIR_CTFE_OUT_OF_MEMORY);
            CHECK(source_fixture_compile_injected && !output.type && !output.reserved && !output.payload);
            CHECK(reference_stats(context).live_bytes==baseline.live_bytes);source_fixture_compile_fail_at=SIZE_MAX;
            CHECK(reference_text_evaluate(&request,mode!=0,&output)==XR_XIR_CTFE_OK);reference_nominal_value(&output,selected,reader);xr_xir_value_drop(&output);
        }
    }
    if (selected->value) { request.limits.output_bytes=(uint64_t)selected->value-1;CHECK(reference_text_evaluate(&request,false,&output)==XR_XIR_CTFE_OUTPUT_LIMIT && !output.type && !output.payload); }
    request.limits.output_bytes=(uint64_t)selected->value;request.limits.steps=1;
    CHECK(reference_text_evaluate(&request,false,&output)==XR_XIR_CTFE_STEP_LIMIT && !output.type && !output.payload);
    request.limits.steps=XR_XIR_CTFE_MAX_STEPS;request.limits.frame_bytes=1;
    CHECK(reference_text_evaluate(&request,false,&output)==XR_XIR_CTFE_FRAME_LIMIT && !output.type && !output.payload);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);request.limits.frame_bytes=XR_XIR_CTFE_MAX_FRAME_BYTES;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK);
    if (modes==2) {
        XrXirCtfeRequest external=request;external.function=reference_named(module,"echo");external.arguments=&output;external.argument_count=1;
        XrXirValue refused={0},unchanged=output;
        CHECK(reference_text_evaluate(&external,false,&refused)==XR_XIR_CTFE_NOT_SUPPORTED && !refused.type && !refused.payload);
        CHECK(reference_text_evaluate(&external,true,&refused)==XR_XIR_CTFE_NOT_SUPPORTED && !refused.type && !refused.payload);
        CHECK(!memcmp(&unchanged,&output,sizeof(output)));reference_nominal_value(&output,selected,reader);
    }
    xr_xir_compile_artifact_free(lowered);reference_nominal_value(&output,selected,reader);xr_xir_value_drop(&output);xr_xir_domain_drop(reader);
    fprintf(stderr,"CTFE nominal %s logical=%lld leaf FI=%zu calls FI=%zu; complete same-owner/live baseline; actual fields/tag, output exact-minus, step/frame and Artifact death physical0\n",selected->name,(long long)selected->value,counts[0],counts[1]);
}
static void reference_nominal_last_owner(const CtfeReferenceCase *selected) {
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&reference_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(reference_source(&context,selected->name,&source,&diagnostic)==XR_XIR_OK);uint8_t identity[32]={0};
    XrXirArtifact *lowered=reference_lower(&context,&source,identity);XrXirValue value={0};XrXirDomain *reader=NULL;
    CHECK(xr_xir_compile_domain_new(context.resources,UINT64_MAX,&reader)==XR_XIR_VALUE_OK);
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_nominal_function(xr_xir_compile_artifact_module(lowered),selected),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,(uint64_t)selected->value}};
    memcpy(request.checked_identity,identity,sizeof(identity));CHECK(reference_text_evaluate(&request,true,&value)==XR_XIR_CTFE_OK);
    xr_xir_compile_artifact_free(lowered);xr_compile_resources_release(context.resources);reference_nominal_value(&value,selected,reader);
    xr_xir_domain_drop(reader);size_t attempts=source_fixture_compile_attempts;source_fixture_compile_fail_at=attempts;
    xr_xir_value_drop(&value);source_fixture_compile_fail_at=SIZE_MAX;
    CHECK(source_fixture_compile_attempts==attempts && !source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"CTFE actual nominal survives Source/Artifact/ledger caller death; final drop allocates nothing physical0\n");
}
#endif // XIR_CTFE_NOMINAL_CASES_H
