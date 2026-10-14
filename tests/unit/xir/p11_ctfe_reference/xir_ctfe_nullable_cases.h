/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ctfe_nullable_cases.h - Independent active variants and owning lifetimes
 */
#ifndef XIR_CTFE_NULLABLE_CASES_H
#define XIR_CTFE_NULLABLE_CASES_H
#include "xir/xxir_nullable.h"
#include "xir/xxir_compile_domain.h"
static void reference_nullable_value(const XrXirValue *value,const CtfeReferenceCase *selected) {
    const XrXirTypeArena *arena=xr_xir_value_arena(value);CHECK(arena && xr_xir_value_valid(value));
    CHECK(xr_xir_type_is_nullable(xr_xir_compile_type_arena_types(arena),(XrXirType)value->type));
    bool some=false;const XrXirValue *payload=NULL;
    CHECK(xr_xir_nullable_view(value,&some,&payload));
    if (!strcmp(selected->name,"nullable_none")) { CHECK(!some && !payload);return; }
    CHECK(some && payload);
    if (!strcmp(selected->name,"nullable_nested") || !strcmp(selected->name,"nullable_some_none")) {
        CHECK(xr_xir_nullable_view(payload,&some,&payload));
        if (!strcmp(selected->name,"nullable_some_none")) { CHECK(!some && !payload);return; }
        CHECK(some && payload);
    }
    if (!strcmp(selected->name,"nullable_tuple")) {
        XrXirValue number={0},text={0};const char *bytes=NULL;size_t length=0;
        CHECK(xr_xir_tuple_get(payload,0,&number)==XR_XIR_VALUE_OK && number.type==XR_XIR_I8 && number.payload==7);
        CHECK(xr_xir_tuple_get(payload,1,&text)==XR_XIR_VALUE_OK && xr_xir_string_view(&text,&bytes,&length));
        CHECK(length==2 && !memcmp(bytes,"ab",2));xr_xir_value_drop(&number);xr_xir_value_drop(&text);
    } else if (!strcmp(selected->name,"nullable_string")) {
        const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(payload,&bytes,&length));
        CHECK(length==3 && !memcmp(bytes,"a\0b",3));
    } else if (!strcmp(selected->name,"nullable_units")) {
        for (uint32_t i=0;i<2;++i) {
            XrXirValue unit={0};CHECK(xr_xir_tuple_get(payload,i,&unit)==XR_XIR_VALUE_OK);
            CHECK(!unit.type && !unit.reserved && !unit.payload);
        }
    } else CHECK(payload->type==XR_XIR_I8 && !payload->reserved && payload->payload==7);
}
static void reference_nullable_type_policy(const XrXirCompileContext *context) {
    XrXirType first=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,second=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1),third=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+2);
    XrXirCallableParameter fields[]={{first,0},{XR_XIR_UNIT,0}};
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_I8},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=fields,.parameter_count=2},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=second}};
    XrXirTypes types={nodes,3,NULL,NULL};XrCompileResourceStats baseline=reference_stats(context);
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_OK);
    CHECK(xr_xir_compile_ctfe_type_admit(context,&types,third)==XR_XIR_CTFE_OK);
    nodes[0].element=third;
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_ctfe_type_admit(context,&types,third)==XR_XIR_CTFE_NOT_SUPPORTED);
    nodes[0].element=XR_XIR_UNIT;
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_ctfe_type_admit(context,&types,third)==XR_XIR_CTFE_NOT_SUPPORTED);
    nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I8,.flags=XR_XIR_CALLABLE_ROOT_NONE};
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_OK);
    CHECK(xr_xir_compile_ctfe_type_admit(context,&types,third)==XR_XIR_CTFE_NOT_SUPPORTED);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    fprintf(stderr,"CTFE Nullable/Tuple canonical mixed type policy; cyclic/illegal Unit element/callable authority refused with live baseline\n");
}
static void reference_nullable(const XrXirCompileContext *context,XrXirSourceResult *source,const CtfeReferenceCase *selected) {
    if (!strcmp(selected->name,"nullable_some")) reference_nullable_type_policy(context);
    const XrXirModule *module=xr_xir_compile_artifact_module(source->checked);
    const XrXirFunction *body=&module->functions[reference_run_function(module)];bool recipe=false;
    for (uint32_t i=0;i<body->instruction_count;++i) {
        CHECK(body->instructions[i].op!=XR_XIR_CALL && body->instructions[i].op!=XR_XIR_CALL_INDIRECT);
        if (body->instructions[i].op==XR_XIR_NULLABLE_NONE || body->instructions[i].op==XR_XIR_NULLABLE_SOME) recipe=true;
    }
    CHECK(recipe);uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_run_function(xr_xir_compile_artifact_module(lowered)),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,(uint64_t)selected->value}};
    memcpy(request.checked_identity,identity,sizeof(identity));XrXirValue output={0};
    XrCompileResourceStats baseline=reference_stats(context);size_t first=source_fixture_compile_attempts;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK);reference_nullable_value(&output,selected);
    size_t sites=source_fixture_compile_attempts-first;CHECK(sites>0);xr_xir_value_drop(&output);
    for (size_t fault=0;fault<sites;++fault) {
        source_fixture_compile_injected=false;source_fixture_compile_fail_at=source_fixture_compile_attempts+fault;
        CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OUT_OF_MEMORY);
        CHECK(source_fixture_compile_injected && !output.type && !output.reserved && !output.payload);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK);reference_nullable_value(&output,selected);xr_xir_value_drop(&output);
    }
    request.limits.output_bytes=(uint64_t)selected->value-1;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OUTPUT_LIMIT && !output.type && !output.payload);
    request.limits.output_bytes=(uint64_t)selected->value;request.limits.steps=1;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_STEP_LIMIT && !output.type && !output.payload);
    request.limits.steps=XR_XIR_CTFE_MAX_STEPS;request.limits.frame_bytes=1;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_FRAME_LIMIT && !output.type && !output.payload);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);request.limits.frame_bytes=XR_XIR_CTFE_MAX_FRAME_BYTES;
    XrXirCtfeCallRequest calls={request,XR_XIR_CTFE_MAX_CALL_DEPTH};
    CHECK(xr_xir_compile_ctfe_calls(&calls,&output)==XR_XIR_CTFE_OK);
    xr_xir_compile_artifact_free(lowered);reference_nullable_value(&output,selected);xr_xir_value_drop(&output);
    fprintf(stderr,"CTFE Nullable logical bytes=%lld; evaluator FI=%zu complete same-owner retry/live baseline; independent variant/recipe; exact/minus-one/step/frame; Artifact death physical0\n",(long long)selected->value,sites);
}
static void reference_nullable_direct(const XrXirCompileContext *context,XrXirSourceResult *source) {
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);XrXirValue output={0};
    XrXirCtfeRequest request={.artifact=lowered,.target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,8}};
    memcpy(request.checked_identity,identity,sizeof(identity));XrCompileResourceStats baseline=reference_stats(context);
    request.function=reference_named(module,"absent");XrXirCtfeCallRequest calls={request,XR_XIR_CTFE_MAX_CALL_DEPTH};
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_NULL_UNWRAP && !output.type && !output.payload);
    CHECK(xr_xir_compile_ctfe_calls(&calls,&output)==XR_XIR_CTFE_NULL_UNWRAP && !output.type && !output.payload);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    request.function=reference_named(module,"present");CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_I8 && output.payload==7);
    output=(XrXirValue){0};request.function=reference_named(module,"identity");request.argument_count=1;
    XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    CHECK(xr_xir_compile_type_arena_new(context,module->types,&arena)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_domain_new(context->resources,UINT64_MAX,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={.arena=arena,.domain=domain,.work=128,.scratch_bytes=65536};XrXirValue argument={0};
    CHECK(xr_xir_nullable_new(module->functions[request.function].parameters[0],NULL,&admission,&argument)==XR_XIR_VALUE_OK);
    request.arguments=&argument;calls=(XrXirCtfeCallRequest){request,XR_XIR_CTFE_MAX_CALL_DEPTH};
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_NOT_SUPPORTED && !output.type && !output.payload);
    CHECK(xr_xir_compile_ctfe_calls(&calls,&output)==XR_XIR_CTFE_NOT_SUPPORTED && !output.type && !output.payload);
    bool some=true;const XrXirValue *payload=NULL;CHECK(xr_xir_nullable_view(&argument,&some,&payload) && !some && !payload);
    xr_xir_value_drop(&argument);xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);xr_xir_compile_artifact_free(lowered);
    fprintf(stderr,"CTFE Nullable unwrap Some/None both entries; failure releases owned frame; foreign arguments refuse without mutation; physical0\n");
}
static void reference_nullable_late_owner(void) {
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&reference_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(reference_source(&context,"nullable_tuple",&source,&diagnostic)==XR_XIR_OK);
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(&context,&source,identity);
    XrXirCtfeCallRequest request={.evaluation={.artifact=lowered,.function=reference_run_function(xr_xir_compile_artifact_module(lowered)),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,4}},.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
    memcpy(request.evaluation.checked_identity,identity,sizeof(identity));XrXirValue value={0};
    CHECK(xr_xir_compile_ctfe_calls(&request,&value)==XR_XIR_CTFE_OK);
    xr_xir_compile_artifact_free(lowered);xr_compile_resources_release(context.resources);
    CtfeReferenceCase selected={"nullable_tuple",XR_XIR_UNIT,4,XR_XIR_OK,NULL,0};reference_nullable_value(&value,&selected);
    size_t attempts=source_fixture_compile_attempts;source_fixture_compile_fail_at=attempts;xr_xir_value_drop(&value);source_fixture_compile_fail_at=SIZE_MAX;
    CHECK(source_fixture_compile_attempts==attempts && !source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"CTFE Nullable/Tuple/String and type arena survive ledger caller death; final drop no allocation physical0\n");
}
#endif // XIR_CTFE_NULLABLE_CASES_H
