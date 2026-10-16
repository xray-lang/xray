/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ctfe_array_cases.h - Independent elements, capacities and owning export
 */
#ifndef XIR_CTFE_ARRAY_CASES_H
#define XIR_CTFE_ARRAY_CASES_H
#include "xir/xxir_array.h"
static void reference_array_type_policy(const XrXirCompileContext *context) {
    XrXirType first=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,second=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1),third=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+2);
    XrXirCallableParameter fields[]={{XR_XIR_I8,0},{XR_XIR_STRING,0}};
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_TUPLE,.parameters=fields,.parameter_count=2},
        {.kind=XR_XIR_TYPE_ARRAY,.element=first},{.kind=XR_XIR_TYPE_ARRAY,.element=second}};
    XrXirTypes types={nodes,3,NULL,NULL};XrXirCtfeTypeShape shape={0};
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_OK);
    CHECK(xr_xir_compile_ctfe_type_shape(context,&types,second,&shape)==XR_XIR_CTFE_OK && shape.dynamic && !shape.pure_arrays);
    CHECK(xr_xir_compile_ctfe_type_shape(context,&types,third,&shape)==XR_XIR_CTFE_OK && shape.dynamic && !shape.pure_arrays);
    nodes[1].element=XR_XIR_I8;
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_OK);
    CHECK(xr_xir_compile_ctfe_type_shape(context,&types,third,&shape)==XR_XIR_CTFE_OK && !shape.dynamic && shape.pure_arrays);
    fprintf(stderr,"CTFE Array type policy: fixed and variable Tuple elements, with pure primitive Array chains\n");
}
static void reference_array_value(const XrXirValue *value,const CtfeReferenceCase *selected,XrXirDomain *domain) {
    const XrXirTypeArena *arena=xr_xir_value_arena(value);CHECK(arena && xr_xir_value_valid(value));
    XrXirValueAdmission admission={.arena=arena,.domain=domain,.work=UINT64_MAX,.scratch_bytes=65536};int64_t length=0,capacity=0;
    CHECK(xr_xir_array_len(value,&admission,&length)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_capacity(value,&admission,&capacity)==XR_XIR_VALUE_OK);
    int64_t expected=!strcmp(selected->name,"array_repeat")?3:!strcmp(selected->name,"array_export")?4:(!strcmp(selected->name,"array_tuple") || !strcmp(selected->name,"array_deep_gap"))?1:
        (!strcmp(selected->name,"array_empty") || !strcmp(selected->name,"array_capacity"))?0:2;
    CHECK(length==expected);
    CHECK(capacity==(!strcmp(selected->name,"array_reserve")?6:!strcmp(selected->name,"array_capacity")?2:length?4:0));
    for (int64_t i=0;i<length;++i) {
        XrXirValue element={0};XrXirFaultDetail fault={0};CHECK(xr_xir_array_get(value,i,&admission,&element,&fault)==XR_XIR_VALUE_OK);
        if (!strcmp(selected->name,"array_deep_gap")) {
            int64_t count=0;CHECK(xr_xir_array_len(&element,&admission,&count)==XR_XIR_VALUE_OK && count==2);
            for (int64_t j=0;j<count;++j) {
                XrXirValue number={0};CHECK(xr_xir_array_get(&element,j,&admission,&number,&fault)==XR_XIR_VALUE_OK);
                bool some=false;const XrXirValue *payload=NULL;CHECK(xr_xir_nullable_view(&number,&some,&payload) && some==(j!=0));
                if (some) CHECK(payload && payload->type==XR_XIR_I8 && payload->payload==7);xr_xir_value_drop(&number);
            }
        } else if (!strcmp(selected->name,"array_nested")) {
            XrXirValueAdmission child=admission;int64_t count=0;CHECK(xr_xir_array_len(&element,&child,&count)==XR_XIR_VALUE_OK && count==(i?1:2));
            for (int64_t j=0;j<count;++j) { XrXirValue number={0};CHECK(xr_xir_array_get(&element,j,&child,&number,&fault)==XR_XIR_VALUE_OK);CHECK(number.type==XR_XIR_I8 && number.payload==(i?3:j+1)); }
        } else if (!strcmp(selected->name,"array_string") || !strcmp(selected->name,"array_path_string_direct")) {
            const char *bytes=NULL;size_t size=0;CHECK(xr_xir_string_view(&element,&bytes,&size));
            CHECK(size==(i?1u:3u) && !memcmp(bytes,i?"z":"a\0b",size));
        } else if (!strcmp(selected->name,"array_tuple")) {
            XrXirValue number={0},text={0};const char *bytes=NULL;size_t size=0;
            CHECK(xr_xir_tuple_get(&element,0,&number)==XR_XIR_VALUE_OK && number.type==XR_XIR_I8 && number.payload==7);
            CHECK(xr_xir_tuple_get(&element,1,&text)==XR_XIR_VALUE_OK && xr_xir_string_view(&text,&bytes,&size) && size==2 && !memcmp(bytes,"ab",2));
            xr_xir_value_drop(&number);xr_xir_value_drop(&text);
        } else if (!strcmp(selected->name,"array_nullable")) {
            bool some=false;const XrXirValue *payload=NULL;CHECK(xr_xir_nullable_view(&element,&some,&payload));
            CHECK(some==(i!=0));if (some) CHECK(payload && payload->type==XR_XIR_I8 && payload->payload==7);
        } else CHECK(element.type==XR_XIR_I8 && element.payload==(!strcmp(selected->name,"array_repeat")?7:
            (!strcmp(selected->name,"array_cell") || !strcmp(selected->name,"array_path_export") || !strcmp(selected->name,"array_path_direct"))?(i?2:7):i+1));
        xr_xir_value_drop(&element);
    }
}
static void reference_array(const XrXirCompileContext *context,XrXirSourceResult *source,const CtfeReferenceCase *selected) {
    if (!strcmp(selected->name,"array_export")) reference_array_type_policy(context);
    const XrXirModule *module=xr_xir_compile_artifact_module(source->checked);
    const XrXirFunction *body=&module->functions[reference_run_function(module)];bool recipe=false;
    for (uint32_t i=0;i<body->instruction_count;++i) {
        CHECK(body->instructions[i].op!=XR_XIR_CALL && body->instructions[i].op!=XR_XIR_CALL_INDIRECT);
        if (body->instructions[i].op==XR_XIR_ARRAY_WITH_CAPACITY ||
            (strstr(selected->name,"_direct") && body->instructions[i].op==XR_XIR_INDEX_PLACE)) recipe=true;
    }
    CHECK(recipe);uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);
    XrXirDomain *reader=NULL;CHECK(xr_xir_compile_domain_new(context->resources,UINT64_MAX,&reader)==XR_XIR_VALUE_OK);
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_run_function(xr_xir_compile_artifact_module(lowered)),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,(uint64_t)selected->value}};
    memcpy(request.checked_identity,identity,sizeof(identity));XrXirValue output={0};
    XrCompileResourceStats baseline=reference_stats(context);size_t first=source_fixture_compile_attempts;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK);size_t sites=source_fixture_compile_attempts-first;
    reference_array_value(&output,selected,reader);CHECK(sites>0);xr_xir_value_drop(&output);
    for (size_t fault=0;fault<sites;++fault) {
        source_fixture_compile_injected=false;source_fixture_compile_fail_at=source_fixture_compile_attempts+fault;
        CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OUT_OF_MEMORY);
        CHECK(source_fixture_compile_injected && !output.type && !output.reserved && !output.payload);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK);reference_array_value(&output,selected,reader);xr_xir_value_drop(&output);
    }
    if (selected->value) { request.limits.output_bytes=(uint64_t)selected->value-1;CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OUTPUT_LIMIT && !output.type && !output.payload); }
    request.limits.output_bytes=(uint64_t)selected->value;request.limits.steps=1;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_STEP_LIMIT && !output.type && !output.payload);
    request.limits.steps=XR_XIR_CTFE_MAX_STEPS;request.limits.frame_bytes=1;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_FRAME_LIMIT && !output.type && !output.payload);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);request.limits.frame_bytes=XR_XIR_CTFE_MAX_FRAME_BYTES;
    XrXirCtfeCallRequest calls={request,XR_XIR_CTFE_MAX_CALL_DEPTH};
    CHECK(xr_xir_compile_ctfe_calls(&calls,&output)==XR_XIR_CTFE_OK);
    if (strstr(selected->name,"array_path_") && strstr(selected->name,"_direct")) {
        reference_array_value(&output,selected,reader);xr_xir_value_drop(&output);
        first=source_fixture_compile_attempts;CHECK(xr_xir_compile_ctfe_calls(&calls,&output)==XR_XIR_CTFE_OK);
        size_t call_sites=source_fixture_compile_attempts-first;xr_xir_value_drop(&output);CHECK(call_sites>0);
        for (size_t fault=0;fault<call_sites;++fault) {
            source_fixture_compile_injected=false;source_fixture_compile_fail_at=source_fixture_compile_attempts+fault;
            CHECK(xr_xir_compile_ctfe_calls(&calls,&output)==XR_XIR_CTFE_OUT_OF_MEMORY);
            CHECK(source_fixture_compile_injected && !output.type && !output.reserved && !output.payload);
            CHECK(reference_stats(context).live_bytes==baseline.live_bytes);source_fixture_compile_fail_at=SIZE_MAX;
            CHECK(xr_xir_compile_ctfe_calls(&calls,&output)==XR_XIR_CTFE_OK);
            reference_array_value(&output,selected,reader);xr_xir_value_drop(&output);
        }
        uint32_t mutation=UINT32_MAX;
        const XrXirFunction *direct=&xr_xir_compile_artifact_module(lowered)->functions[request.function];
        for (uint32_t i=0;i<direct->instruction_count;++i)
            if (direct->instructions[i].op==XR_XIR_ARRAY_SET || direct->instructions[i].op==XR_XIR_ARRAY_PUSH) mutation=i;
        CHECK(mutation!=UINT32_MAX);request.limits.steps=(uint64_t)mutation+1;calls.evaluation=request;
        CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_STEP_LIMIT && !output.type && !output.payload);
        CHECK(xr_xir_compile_ctfe_calls(&calls,&output)==XR_XIR_CTFE_STEP_LIMIT && !output.type && !output.payload);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
        request.limits.steps=XR_XIR_CTFE_MAX_STEPS;calls.evaluation=request;
        if (!strcmp(selected->name,"array_path_string_direct")) {
            request.function=reference_named(xr_xir_compile_artifact_module(lowered),"fail");calls.evaluation=request;
            CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_BOUNDS && !output.type && !output.payload);
            CHECK(xr_xir_compile_ctfe_calls(&calls,&output)==XR_XIR_CTFE_BOUNDS && !output.type && !output.payload);
            CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
            request.function=reference_run_function(xr_xir_compile_artifact_module(lowered));calls.evaluation=request;
        }
        CHECK(xr_xir_compile_ctfe_calls(&calls,&output)==XR_XIR_CTFE_OK);
        fprintf(stderr,"CTFE private index path: calls FI=%zu complete same-owner retry/live baseline; post-mutation step failure both entries\n",call_sites);
    }
    xr_xir_compile_artifact_free(lowered);reference_array_value(&output,selected,reader);xr_xir_value_drop(&output);xr_xir_domain_drop(reader);
    fprintf(stderr,"CTFE Array logical bytes=%lld; evaluator FI=%zu exact same-owner retry/live baseline; independent elements/capacity recipes; output/step/Artifact death physical0\n",(long long)selected->value,sites);
}
static void reference_array_late_owner(const char *name) {
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&reference_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(reference_source(&context,name,&source,&diagnostic)==XR_XIR_OK);
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(&context,&source,identity);
    XrXirCtfeCallRequest request={.evaluation={.artifact=lowered,.function=reference_run_function(xr_xir_compile_artifact_module(lowered)),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,4}},.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
    memcpy(request.evaluation.checked_identity,identity,sizeof(identity));XrXirValue value={0};
    CHECK(xr_xir_compile_ctfe_calls(&request,&value)==XR_XIR_CTFE_OK);
    XrCompileResources *resources=context.resources;xr_xir_compile_artifact_free(lowered);xr_compile_resources_release(resources);
    XrXirDomain *reader=NULL;CHECK(xr_xir_compile_domain_new(resources,UINT64_MAX,&reader)==XR_XIR_VALUE_OK);
    CtfeReferenceCase selected={"array_string",XR_XIR_UNIT,4,XR_XIR_OK,NULL,0};reference_array_value(&value,&selected,reader);xr_xir_domain_drop(reader);
    size_t attempts=source_fixture_compile_attempts;source_fixture_compile_fail_at=attempts;xr_xir_value_drop(&value);source_fixture_compile_fail_at=SIZE_MAX;
    CHECK(source_fixture_compile_attempts==attempts && !source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"CTFE Array/String metadata survive Source/Artifact/ledger caller death; final drop allocates nothing physical0\n");
}
#endif // XIR_CTFE_ARRAY_CASES_H
