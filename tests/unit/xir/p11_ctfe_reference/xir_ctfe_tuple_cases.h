/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ctfe_tuple_cases.h - Independent ordered values and logical export limits
 */
#ifndef XIR_CTFE_TUPLE_CASES_H
#define XIR_CTFE_TUPLE_CASES_H
#include "xir/xxir_tuple.h"
#include "xir/xxir_types.h"
#include "xir/xxir_type_arena.h"
static void reference_tuple_value(const XrXirValue *value,const CtfeReferenceCase *selected) {
    const XrXirTypeArena *arena=xr_xir_value_arena(value);CHECK(arena && xr_xir_value_valid(value));
    const XrXirTypeNode *tuple=xr_xir_tuple_signature(xr_xir_compile_type_arena_types(arena),(XrXirType)value->type);
    CHECK(tuple && tuple->parameter_count==(!strcmp(selected->name,"tuple_export")?3u:2u));
    XrXirValue a={0},b={0},unit={0};CHECK(xr_xir_tuple_get(value,0,&a)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_tuple_get(value,1,&b)==XR_XIR_VALUE_OK);
    if (!strcmp(selected->name,"float_tuple")) {
        CHECK(a.type==XR_XIR_F32 && !a.reserved && a.payload==INT64_C(0x40600000));
        CHECK(b.type==XR_XIR_F32 && !b.reserved && b.payload==INT64_C(0x3fe00000));
    } else if (!strcmp(selected->name,"tuple_oversize")) CHECK(a.type==XR_XIR_I64 && a.payload==41 && b.type==XR_XIR_BOOL && b.payload==1);
    else if (!strcmp(selected->name,"tuple_units")) CHECK(!a.type && !a.reserved && !a.payload && !b.type && !b.reserved && !b.payload);
    else {
        if (!strcmp(selected->name,"tuple_nested")) {
            XrXirValue number={0};CHECK(xr_xir_tuple_get(&a,0,&number)==XR_XIR_VALUE_OK);
            CHECK(number.type==XR_XIR_I8 && !number.reserved && number.payload==7);
            CHECK(xr_xir_tuple_get(&a,1,&unit)==XR_XIR_VALUE_OK && !unit.type && !unit.reserved && !unit.payload);
        } else CHECK(a.type==XR_XIR_I8 && !a.reserved && a.payload==7);
        const char *bytes=NULL;size_t length=0;
        bool nested=!strcmp(selected->name,"tuple_nested");
        CHECK(xr_xir_string_view(&b,&bytes,&length) && length==(nested?3u:2u) && !memcmp(bytes,nested?"a\0b":"ab",length));
        if (!strcmp(selected->name,"tuple_export")) CHECK(xr_xir_tuple_get(value,2,&unit)==XR_XIR_VALUE_OK && !unit.type && !unit.reserved && !unit.payload);
    }
    xr_xir_value_drop(&a);xr_xir_value_drop(&b);xr_xir_value_drop(&unit);
}
static void reference_tuple_type_policy(const XrXirCompileContext *context) {
    const XrXirType first=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,second=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1);
    XrXirCallableParameter ordered[]={{first,0},{XR_XIR_UNIT,0}},leaf[]={{XR_XIR_I8,0},{XR_XIR_STRING,0}};
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_TUPLE,.parameters=leaf,.parameter_count=2},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=ordered,.parameter_count=2}};
    XrXirTypes types={nodes,2,NULL,NULL};XrCompileResourceStats baseline=reference_stats(context);
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_OK);
    CHECK(xr_xir_compile_ctfe_type_admit(context,&types,second)==XR_XIR_CTFE_OK);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    ordered[0].type=second;
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_ctfe_type_admit(context,&types,second)==XR_XIR_CTFE_NOT_SUPPORTED);
    ordered[0].type=first;nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_NONE};
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_OK);
    CHECK(xr_xir_compile_ctfe_type_admit(context,&types,second)==XR_XIR_CTFE_NOT_SUPPORTED);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    fprintf(stderr,"CTFE actual type policy admits canonical ordered Tuple descriptors; cycles and nested callable authority reject; live baseline preserved\n");
}
static void reference_tuple(const XrXirCompileContext *context,XrXirSourceResult *source,const CtfeReferenceCase *selected) {
    reference_tuple_type_policy(context);
    const XrXirModule *module=xr_xir_compile_artifact_module(source->checked);
    const XrXirFunction *body=&module->functions[reference_run_function(module)];bool recipe=false;
    for (uint32_t i=0;i<body->instruction_count;++i) {
        CHECK(body->instructions[i].op!=XR_XIR_CALL && body->instructions[i].op!=XR_XIR_CALL_INDIRECT);
        if (body->instructions[i].op==XR_XIR_TUPLE_NEW) recipe=true;
    }
    CHECK(recipe);
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_run_function(xr_xir_compile_artifact_module(lowered)),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,(uint64_t)selected->value}};
    memcpy(request.checked_identity,identity,sizeof(identity));XrXirValue output={0};
    XrCompileResourceStats baseline=reference_stats(context);size_t first=source_fixture_compile_attempts;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK);reference_tuple_value(&output,selected);
    size_t sites=source_fixture_compile_attempts-first;CHECK(sites>0);xr_xir_value_drop(&output);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    for (size_t fault=0;fault<sites;++fault) {
        source_fixture_compile_injected=false;source_fixture_compile_fail_at=source_fixture_compile_attempts+fault;
        CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OUT_OF_MEMORY);
        CHECK(source_fixture_compile_injected && !output.type && !output.reserved && !output.payload);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK);reference_tuple_value(&output,selected);xr_xir_value_drop(&output);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    }
    if (selected->value) {
        request.limits.output_bytes=(uint64_t)selected->value-1;
        CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OUTPUT_LIMIT);
        CHECK(!output.type && !output.reserved && !output.payload && reference_stats(context).live_bytes==baseline.live_bytes);
    }
    request.limits.output_bytes=(uint64_t)selected->value;
    request.limits.steps=1;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_STEP_LIMIT);
    CHECK(!output.type && !output.reserved && !output.payload && reference_stats(context).live_bytes==baseline.live_bytes);
    request.limits.steps=XR_XIR_CTFE_MAX_STEPS;request.limits.frame_bytes=1;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_FRAME_LIMIT);
    CHECK(!output.type && !output.reserved && !output.payload && reference_stats(context).live_bytes==baseline.live_bytes);
    request.limits.frame_bytes=XR_XIR_CTFE_MAX_FRAME_BYTES;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK);
    if (!strcmp(selected->name,"tuple_generic")) {
        const XrXirModule *lowered_module=xr_xir_compile_artifact_module(lowered);uint32_t parameter=UINT32_MAX;
        for (uint32_t f=0;f<lowered_module->function_count;++f)
            if (lowered_module->functions[f].parameter_count==1 && lowered_module->functions[f].parameters[0]==(XrXirType)output.type) parameter=f;
        CHECK(parameter!=UINT32_MAX);XrXirValue refused={0};XrXirCtfeRequest entry=request;
        entry.function=parameter;entry.arguments=&output;entry.argument_count=1;
        CHECK(xr_xir_compile_ctfe_leaf(&entry,&refused)==XR_XIR_CTFE_OK);
        reference_tuple_value(&refused,selected);xr_xir_value_drop(&refused);reference_tuple_value(&output,selected);
        XrXirCtfeCallRequest calls={entry,XR_XIR_CTFE_MAX_CALL_DEPTH};
        CHECK(xr_xir_compile_ctfe_calls(&calls,&refused)==XR_XIR_CTFE_OK);
        reference_tuple_value(&refused,selected);xr_xir_value_drop(&refused);reference_tuple_value(&output,selected);
    }
    xr_xir_compile_artifact_free(lowered);
    reference_tuple_value(&output,selected);xr_xir_value_drop(&output);
    fprintf(stderr,"CTFE Tuple export logical bytes=%lld; evaluator FI=%zu complete same-owner retry; independent recipe/Unit/nested owner/Artifact death; step/frame failure cleanup; exact/minus-one; physical0 after last owner\n",(long long)selected->value,sites);
}
static void reference_tuple_late_owner(void) {
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&reference_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(reference_source(&context,"tuple_export",&source,&diagnostic)==XR_XIR_OK);
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(&context,&source,identity);
    XrXirCtfeCallRequest request={.evaluation={.artifact=lowered,
        .function=reference_run_function(xr_xir_compile_artifact_module(lowered)),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,3}},.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
    memcpy(request.evaluation.checked_identity,identity,sizeof(identity));XrXirValue value={0};
    CHECK(xr_xir_compile_ctfe_calls(&request,&value)==XR_XIR_CTFE_OK);
    xr_xir_compile_artifact_free(lowered);xr_compile_resources_release(context.resources);context.resources=NULL;
    CtfeReferenceCase selected={"tuple_export",XR_XIR_UNIT,3,XR_XIR_OK,NULL,0};reference_tuple_value(&value,&selected);
    size_t attempts=source_fixture_compile_attempts;source_fixture_compile_fail_at=attempts;
    xr_xir_value_drop(&value);source_fixture_compile_fail_at=SIZE_MAX;
    CHECK(source_fixture_compile_attempts==attempts && !source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"CTFE Tuple nested fields/type arena survive Source/Artifact/ledger caller death; final drop no allocation physical0\n");
}
#endif // XIR_CTFE_TUPLE_CASES_H
