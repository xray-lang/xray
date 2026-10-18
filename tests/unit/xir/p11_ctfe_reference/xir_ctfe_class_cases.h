/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ctfe_class_cases.h - Private identity and complete receiving fault controls
 */
#ifndef XIR_CTFE_CLASS_CASES_H
#define XIR_CTFE_CLASS_CASES_H
#include "xir/xxir_class.h"
static void reference_class_foreign(const XrXirCompileContext *context,const XrXirCtfeRequest *request) {
    const XrXirModule *module=xr_xir_compile_artifact_module(request->artifact);
    uint32_t accept=reference_named(module,"accept_box");XrXirType type=module->functions[accept].parameters[0];
    XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    CHECK(xr_xir_compile_type_arena_new(context,module->types,&arena)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_domain_new(context->resources,UINT64_MAX,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={.arena=arena,.domain=domain,.work=UINT64_MAX,.scratch_bytes=65536};
    XrXirValue field={XR_XIR_I64,0,42},array={0},identity={0},output={0};
    const XrXirTypeNode *node=xr_xir_type_node(module->types,type);
    CHECK(node && node->nominal.field_count==1);
    CHECK(xr_xir_array_new(node->nominal.fields[0],&field,1,&admission,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_class_new(type,&array,1,&admission,&identity)==XR_XIR_VALUE_OK);XrXirValue saved=identity;
    XrXirCtfeRequest external=*request;external.function=accept;external.arguments=&identity;external.argument_count=1;
    XrCompileResourceStats baseline=reference_stats(context);
    CHECK(reference_text_evaluate(&external,false,&output)==XR_XIR_CTFE_NOT_SUPPORTED && !output.type && !output.payload);
    CHECK(reference_text_evaluate(&external,true,&output)==XR_XIR_CTFE_NOT_SUPPORTED && !output.type && !output.payload);
    CHECK(!memcmp(&saved,&identity,sizeof(saved)) && reference_stats(context).live_bytes==baseline.live_bytes);
    xr_xir_value_drop(&identity);xr_xir_value_drop(&array);xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);
    for (uint32_t i=0;i<2;++i) {
        external=*request;external.function=reference_named(module,i?"make_tuple":"make_box");baseline=reference_stats(context);
        CHECK(reference_text_evaluate(&external,true,&output)==XR_XIR_CTFE_NOT_SUPPORTED && !output.type && !output.payload);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    }
    fprintf(stderr,"CTFE authentic external Class and Class-bearing exports refused without receiving authority physical0\n");
}
static void reference_class_fault(const XrXirCheckedPacket *wire,const uint8_t *identity,size_t fault,size_t sites) {
    size_t held=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&reference_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrXirArtifact *closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&context,wire->bytes,wire->length,&closed,NULL)==XR_XIR_OK);
    uint8_t digest[32]={0};CHECK(xr_compile_resources_work(context.resources,wire->length)==XR_COMPILE_RESOURCE_OK);
    xr_sha256(wire->bytes,wire->length,digest);CHECK(!memcmp(digest,identity,32));
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    /* Lowering publishes a verified artifact. Both evaluator calls below
     * independently repeat complete verification on this original owner. */
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_run_function(xr_xir_compile_artifact_module(lowered)),.target=target,
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,8}};
    memcpy(request.checked_identity,identity,32);XrXirValue output={0};XrCompileResources *same=context.resources;
    XrCompileResourceStats baseline=reference_stats(&context);size_t start=source_fixture_compile_attempts;
    source_fixture_compile_injected=false;source_fixture_compile_fail_at=start+fault;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OUT_OF_MEMORY);
    CHECK(source_fixture_compile_injected && source_fixture_compile_attempts==start+fault+1 && !output.type && !output.payload);
    CHECK(reference_stats(&context).live_bytes==baseline.live_bytes);source_fixture_compile_fail_at=SIZE_MAX;
    XrCompileResourceStats failed=reference_stats(&context);start=source_fixture_compile_attempts;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_I64 && output.payload==42);
    CHECK(source_fixture_compile_attempts-start==sites);xr_xir_value_drop(&output);XrCompileResourceStats after=reference_stats(&context);
    CHECK(context.resources==same && after.live_bytes==baseline.live_bytes && after.work>=failed.work && after.allocated_bytes>=failed.allocated_bytes);
    CHECK(after.work<=reference_limits.work && after.allocated_bytes<=reference_limits.allocated_bytes && after.peak_bytes<=reference_limits.live_bytes);
    xr_xir_compile_artifact_free(lowered);xr_compile_resources_release(context.resources);
    CHECK(source_fixture_compile_live==held && source_fixture_compile_bytes==bytes);
}
static void reference_class_direct(const XrXirCompileContext *context,XrXirSourceResult *source) {
    XrXirCheckedPacket wire={0};XrXirArtifact *closed=NULL;
    CHECK(xr_xir_compile_specialize(source->checked,&closed,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(closed,&wire,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);XrXirValue output={0};
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t run=reference_run_function(module),projects=0;
    for (uint32_t i=0;i<module->functions[run].instruction_count;++i)
        if (module->functions[run].instructions[i].op==XR_XIR_CELL_PROJECT) ++projects;
    CHECK(projects==1);XrXirCtfeRequest request={.artifact=lowered,.function=run,.target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,8}};
    memcpy(request.checked_identity,identity,32);XrCompileResourceStats baseline=reference_stats(context);size_t start=source_fixture_compile_attempts;
    CHECK(reference_text_evaluate(&request,false,&output)==XR_XIR_CTFE_NOT_LEAF && !output.type && !output.payload);
    start=source_fixture_compile_attempts;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_I64 && output.payload==42);
    size_t sites=source_fixture_compile_attempts-start;CHECK(sites>0);xr_xir_value_drop(&output);
    for (size_t fault=0;fault<sites;++fault) {
        reference_class_fault(&wire,identity,fault,sites);
        if (!(fault%32) || fault+1==sites) fprintf(stderr,"CTFE Class complete FI ordinal=%zu/%zu physical0\n",fault+1,sites);
    }
    uint64_t steps=0;
    for (;;++steps) {
        CHECK(steps<XR_XIR_CTFE_MAX_STEPS);request.limits.steps=steps;
        XrXirCtfeStatus status=reference_text_evaluate(&request,true,&output);
        if (status==XR_XIR_CTFE_OK) { CHECK(output.type==XR_XIR_I64 && output.payload==42);xr_xir_value_drop(&output);break; }
        CHECK(status==XR_XIR_CTFE_STEP_LIMIT && !output.type && !output.payload);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    }
    request.limits.steps=XR_XIR_CTFE_MAX_STEPS;uint32_t depth=1;
    for (;;++depth) {
        XrXirCtfeCallRequest limited={request,depth};CHECK(depth<=XR_XIR_CTFE_MAX_CALL_DEPTH);
        XrXirCtfeStatus status=xr_xir_compile_ctfe_calls(&limited,&output);
        if (status==XR_XIR_CTFE_OK) { CHECK(output.type==XR_XIR_I64 && output.payload==42);xr_xir_value_drop(&output);break; }
        CHECK(status==XR_XIR_CTFE_DEPTH_LIMIT && !output.type && !output.payload);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    }
    fprintf(stderr,"CTFE Class depth prefixes1..%u preserve empty output and exact parent loans\n",depth);
    /* Retained identity keys share the same explicit frame quota as live
     * activations and scratch. Find the exact bound without raising the cap. */
    uint64_t low=0,high=XR_XIR_CTFE_MAX_FRAME_BYTES;
    while (low<high) {
        uint64_t middle=low+(high-low)/2;request.limits.frame_bytes=middle;
        XrXirCtfeStatus status=reference_text_evaluate(&request,true,&output);
        if (status==XR_XIR_CTFE_OK) { xr_xir_value_drop(&output);high=middle; }
        else { CHECK((status==XR_XIR_CTFE_FRAME_LIMIT || status==XR_XIR_CTFE_BUDGET) && !output.type && !output.payload);low=middle+1; }
    }
    CHECK(low);request.limits.frame_bytes=low;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_I64 && output.payload==42);xr_xir_value_drop(&output);
    request.limits.frame_bytes=low-1;
    XrXirCtfeStatus short_frame=reference_text_evaluate(&request,true,&output);
    CHECK((short_frame==XR_XIR_CTFE_FRAME_LIMIT || short_frame==XR_XIR_CTFE_BUDGET) && !output.type && !output.payload);
    request.limits.frame_bytes=XR_XIR_CTFE_MAX_FRAME_BYTES;request.limits.output_bytes=7;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OUTPUT_LIMIT && !output.type && !output.payload);
    request.limits.output_bytes=8;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_I64 && output.payload==42);xr_xir_value_drop(&output);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);uint32_t completed=0;
    for (;;) {
        XrXirCtfeStatus status=reference_text_evaluate(&request,true,&output);
        if (status!=XR_XIR_CTFE_OK) { CHECK(status==XR_XIR_CTFE_BUDGET && !output.type && !output.payload);break; }
        CHECK(output.type==XR_XIR_I64 && output.payload==42);xr_xir_value_drop(&output);++completed;
    }
    uint64_t spent=reference_stats(context).work;CHECK(completed && spent>reference_limits.work-8000000 && spent<=reference_limits.work);
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_BUDGET && !output.type && !output.payload);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);xr_xir_compile_artifact_free(lowered);xr_xir_compile_checked_packet_free(&wire);
    fprintf(stderr,"CTFE actual Class root with cleanup and projected ref: complete FI=%zu per-job same-owner/live, all step prefixes0..%llu, simultaneous child/argument scratch frame exact%llu/minus-one status%u, output8/minus7, canonical roots restored physical0; same-owner exhaustion work%llu/128000000 after%u successes, retry BUDGET\n",
        sites,(unsigned long long)steps,(unsigned long long)low,(unsigned)short_frame,(unsigned long long)spent,completed);
}
static void reference_class_string(const XrXirValue *value) {
    const char *text=NULL;size_t length=0;
    CHECK(xr_xir_string_view(value,&text,&length) && length==2 && !memcmp(text,"ab",2));
}
static void reference_class_owning(const XrXirCompileContext *context,XrXirSourceResult *source) {
    XrXirCheckedPacket wire={0};CHECK(xr_xir_compile_checked_write(source->checked,&wire,NULL)==XR_XIR_OK);
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);XrXirValue output={0};
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_run_function(xr_xir_compile_artifact_module(lowered)),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,2}};
    memcpy(request.checked_identity,identity,32);XrCompileResourceStats baseline=reference_stats(context);
    size_t start=source_fixture_compile_attempts;CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK);
    reference_class_string(&output);size_t sites=source_fixture_compile_attempts-start;CHECK(sites);xr_xir_value_drop(&output);
    for (size_t fault=0;fault<sites;++fault) reference_calls_fault(&wire,identity,fault,sites,"ab");
    uint64_t steps=0;
    for (;;++steps) {
        CHECK(steps<XR_XIR_CTFE_MAX_STEPS);request.limits.steps=steps;
        XrXirCtfeStatus status=reference_text_evaluate(&request,true,&output);
        if (status==XR_XIR_CTFE_OK) { reference_class_string(&output);xr_xir_value_drop(&output);break; }
        CHECK(status==XR_XIR_CTFE_STEP_LIMIT && !output.type && !output.payload);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    }
    request.limits.steps=XR_XIR_CTFE_MAX_STEPS;request.limits.output_bytes=1;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OUTPUT_LIMIT && !output.type && !output.payload);
    request.limits.output_bytes=2;CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK);
    reference_class_string(&output);xr_xir_value_drop(&output);CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_checked_packet_free(&wire);
    fprintf(stderr,"CTFE Class owning String: FI=%zu complete per-job same-owner/live; all step prefixes0..%llu; output2/minus1 physical0\n",
        sites,(unsigned long long)steps);
}
static void reference_class_last_owner(void) {
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&reference_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(reference_source(&context,"class_owning_direct",&source,&diagnostic)==XR_XIR_OK);
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(&context,&source,identity);
    xr_xir_compile_source_result_free(&source);XrXirValue output={0};
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_run_function(xr_xir_compile_artifact_module(lowered)),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,2}};
    memcpy(request.checked_identity,identity,32);CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK);
    xr_xir_compile_artifact_free(lowered);xr_compile_resources_release(context.resources);reference_class_string(&output);
    size_t attempts=source_fixture_compile_attempts;source_fixture_compile_fail_at=attempts;xr_xir_value_drop(&output);
    source_fixture_compile_fail_at=SIZE_MAX;
    CHECK(source_fixture_compile_attempts==attempts && !source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"CTFE Class String survives Source/Artifact/ledger caller death; final drop allocates nothing physical0\n");
}
static void reference_class_cycle(const XrXirCompileContext *context,XrXirSourceResult *source) {
    XrXirCheckedPacket wire={0};CHECK(xr_xir_compile_checked_write(source->checked,&wire,NULL)==XR_XIR_OK);
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);XrXirValue output={0};
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_named(xr_xir_compile_artifact_module(lowered),"unhandled"),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,8}};
    memcpy(request.checked_identity,identity,32);XrCompileResourceStats baseline=reference_stats(context);size_t start=source_fixture_compile_attempts;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_NOT_SUPPORTED && !output.type && !output.payload);
    size_t sites=source_fixture_compile_attempts-start;CHECK(sites);
    for (size_t fault=0;fault<sites;++fault) reference_panic_uncaught_fault(&wire,identity,fault,sites,XR_XIR_CTFE_NOT_SUPPORTED);
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_NOT_SUPPORTED && !output.type && !output.payload);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_checked_packet_free(&wire);
    fprintf(stderr,"CTFE actual Class backedge refusal FI=%zu complete per-job same-owner retry, both identities released physical0\n",sites);
}
static void reference_class_authority(const XrXirCompileContext *context,XrXirSourceResult *source) {
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_run_function(xr_xir_compile_artifact_module(lowered)),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,8}};
    memcpy(request.checked_identity,identity,32);reference_class_foreign(context,&request);xr_xir_compile_artifact_free(lowered);
}
static void reference_class_work(const XrXirCompileContext *context,XrXirSourceResult *source) {
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);XrXirValue output={0};
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_run_function(xr_xir_compile_artifact_module(lowered)),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,8}};
    memcpy(request.checked_identity,identity,32);XrCompileResourceStats baseline=reference_stats(context);
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_BUDGET && !output.type && !output.payload);
    XrCompileResourceStats spent=reference_stats(context);
    CHECK(spent.live_bytes==baseline.live_bytes && spent.work>reference_limits.work-200000 && spent.work<=reference_limits.work);
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_BUDGET && !output.type && !output.payload);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);xr_xir_compile_artifact_free(lowered);
    fprintf(stderr,"CTFE Class128 actual fields receiving pressure work=%llu/128000000 before10000 iterations; retry BUDGET physical0\n",(unsigned long long)spent.work);
}
#endif // XIR_CTFE_CLASS_CASES_H
