/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ctfe_function_cases.h - Independent internal callable ownership controls
 */
#ifndef XIR_CTFE_FUNCTION_CASES_H
#define XIR_CTFE_FUNCTION_CASES_H

typedef struct ReferenceForeignFunction {
    const XrXirCompileContext *context;
    XrXirType type;
    uint32_t entry,admissions,releases;
} ReferenceForeignFunction;
static void reference_foreign_function_release(void *pointer) { ++((ReferenceForeignFunction *)pointer)->releases; }
static XrXirValueStatus reference_foreign_function_admit(void *pointer,const XrXirFunctionBinding *binding,
    XrXirType type,uint64_t *work) {
    ReferenceForeignFunction *owner=pointer;
    if (*work<8 || xr_compile_resources_work(owner->context->resources,8)!=XR_COMPILE_RESOURCE_OK) return XR_XIR_VALUE_LIMIT;
    *work-=8;++owner->admissions;
    return binding->owner==owner && binding->entry==owner->entry && !binding->capture_count && type==owner->type?
        XR_XIR_VALUE_OK:XR_XIR_VALUE_BAD_ARGUMENT;
}
static void reference_function_foreign(const XrXirCompileContext *context,XrXirCtfeRequest *request) {
    const XrXirModule *module=xr_xir_compile_artifact_module(request->artifact);
    uint32_t target=reference_named(module,"ignore_callback");XrXirType type=module->functions[target].parameters[0];
    ReferenceForeignFunction owner={context,type,reference_named(module,"supply"),0,0};
    XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    CHECK(xr_xir_compile_type_arena_new(context,module->types,&arena)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_domain_new(context->resources,UINT64_MAX,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={.arena=arena,.domain=domain,.function=reference_foreign_function_admit,.context=&owner,
        .work=UINT64_MAX,.scratch_bytes=65536};
    XrXirFunctionBinding binding={&owner,reference_foreign_function_release,owner.entry,NULL,0};XrXirValue input={0},output={0};
    CHECK(xr_xir_function_new(domain,arena,type,&binding,&admission,&input)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_valid(&input) && owner.admissions && !owner.releases);XrXirValue unchanged=input;
    XrCompileResourceStats baseline=reference_stats(context);uint32_t admissions=owner.admissions;
    XrXirCtfeRequest external=*request;external.function=target;external.arguments=&input;external.argument_count=1;
    CHECK(reference_text_evaluate(&external,false,&output)==XR_XIR_CTFE_NOT_SUPPORTED && !output.type && !output.payload);
    CHECK(reference_text_evaluate(&external,true,&output)==XR_XIR_CTFE_NOT_SUPPORTED && !output.type && !output.payload);
    CHECK(!memcmp(&unchanged,&input,sizeof(input)) && owner.admissions==admissions && !owner.releases);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(domain);
    size_t attempts=source_fixture_compile_attempts;source_fixture_compile_fail_at=attempts;xr_xir_value_drop(&input);
    source_fixture_compile_fail_at=SIZE_MAX;CHECK(owner.releases==1 && source_fixture_compile_attempts==attempts);
}
static void reference_function_fault(const XrXirCheckedPacket *wire,const uint8_t *identity,size_t fault,size_t sites) {
    size_t held=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&reference_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_read(&context,wire->bytes,wire->length,&received,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    CHECK(xr_xir_compile_checked_write(closed,&packet,NULL)==XR_XIR_OK);uint8_t digest[32]={0};
    CHECK(xr_compile_resources_work(context.resources,packet.length)==XR_COMPILE_RESOURCE_OK);xr_sha256(packet.bytes,packet.length,digest);
    CHECK(!memcmp(digest,identity,sizeof(digest)));xr_xir_compile_checked_packet_free(&packet);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
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
    CHECK(source_fixture_compile_attempts-start==sites);xr_xir_value_drop(&output);
    XrCompileResourceStats after=reference_stats(&context);
    CHECK(context.resources==same && after.live_bytes==baseline.live_bytes && after.work>=failed.work && after.allocated_bytes>=failed.allocated_bytes);
    CHECK(after.work<=reference_limits.work && after.allocated_bytes<=reference_limits.allocated_bytes && after.peak_bytes<=reference_limits.live_bytes);
    xr_xir_compile_artifact_free(lowered);xr_compile_resources_release(context.resources);
    CHECK(source_fixture_compile_live==held && source_fixture_compile_bytes==bytes);
}
static void reference_function_cycle(const XrXirCompileContext *context,XrXirSourceResult *source) {
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);XrXirValue output={0};
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_run_function(xr_xir_compile_artifact_module(lowered)),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,8}};
    memcpy(request.checked_identity,identity,32);XrCompileResourceStats baseline=reference_stats(context);
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_NOT_SUPPORTED && !output.type && !output.payload);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_NOT_SUPPORTED && !output.type && !output.payload);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);xr_xir_compile_artifact_free(lowered);
    fprintf(stderr,"CTFE actual Function/Cell back-edge refused before write twice on same owner; output/live preserved and every lease released physical0\n");
}
static void reference_function_direct(const XrXirCompileContext *context,XrXirSourceResult *source) {
    XrXirCheckedPacket wire={0};CHECK(xr_xir_compile_checked_write(source->checked,&wire,NULL)==XR_XIR_OK);
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);XrXirValue output={0};
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_run_function(module),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,8}};
    memcpy(request.checked_identity,identity,sizeof(identity));reference_function_foreign(context,&request);
    XrCompileResourceStats baseline=reference_stats(context);XrCompileResources *same=context->resources;
    CHECK(reference_text_evaluate(&request,false,&output)==XR_XIR_CTFE_NOT_LEAF && !output.type && !output.payload);
    size_t first=source_fixture_compile_attempts;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_I64 && output.payload==42);
    size_t sites=source_fixture_compile_attempts-first;CHECK(sites>1);xr_xir_value_drop(&output);
    fprintf(stderr,"Function evaluator census sites=%zu work=%llu allocated=%llu\n",sites,
        (unsigned long long)reference_stats(context).work,(unsigned long long)reference_stats(context).allocated_bytes);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    /* Fault scenarios are independent jobs, exactly as whole-producer trials.
     * Each failed evaluation and retry retain that job's original finite owner. */
    for (size_t fault=0;fault<sites;++fault) reference_function_fault(&wire,identity,fault,sites);
    CHECK(context->resources==same && reference_stats(context).live_bytes==baseline.live_bytes);
    request.limits.output_bytes=7;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OUTPUT_LIMIT && !output.type && !output.payload);
    request.limits.output_bytes=8;request.limits.frame_bytes=1;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_FRAME_LIMIT && !output.type && !output.payload);
    request.limits.frame_bytes=XR_XIR_CTFE_MAX_FRAME_BYTES;request.function=reference_named(module,"step");
    const XrXirFunction *body=&module->functions[request.function];uint32_t reference=UINT32_MAX;
    for (uint32_t i=0;i<body->instruction_count;++i) {
        if (body->instructions[i].op==XR_XIR_FUNCTION_REF) { reference=i;break; }
        CHECK(body->instructions[i].op!=XR_XIR_CALL && body->instructions[i].op!=XR_XIR_CALL_INDIRECT &&
            body->instructions[i].op!=XR_XIR_JUMP && body->instructions[i].op!=XR_XIR_BRANCH);
    }
    CHECK(reference!=UINT32_MAX);request.limits.steps=(uint64_t)reference+1;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_STEP_LIMIT && !output.type && !output.payload);
    request.limits.steps=XR_XIR_CTFE_MAX_STEPS;XrXirCtfeCallRequest shallow={request,1};
    CHECK(xr_xir_compile_ctfe_calls(&shallow,&output)==XR_XIR_CTFE_DEPTH_LIMIT && !output.type && !output.payload);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_I64 && output.payload==42);
    xr_xir_value_drop(&output);request.function=reference_run_function(module);uint32_t completed=0;
    for (;;) {
        XrXirCtfeStatus status=reference_text_evaluate(&request,true,&output);
        if (status!=XR_XIR_CTFE_OK) { CHECK(status==XR_XIR_CTFE_BUDGET && !output.type && !output.payload);break; }
        CHECK(output.type==XR_XIR_I64 && output.payload==42);xr_xir_value_drop(&output);++completed;
    }
    XrCompileResourceStats exhausted=reference_stats(context);
    CHECK(completed && exhausted.work>=reference_limits.work-65536 && exhausted.work<=reference_limits.work);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes && context->resources==same);
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_BUDGET && !output.type && !output.payload);
    fprintf(stderr,"Function cumulative same-owner exhaustion: work=%llu/128000000, successes=%u; spent work never reset; retry remains BUDGET physical0\n",(unsigned long long)exhausted.work,completed);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_checked_packet_free(&wire);
    fprintf(stderr,"CTFE internal Function: evaluator FI=%zu complete same-owner/live; cross-frame private Cell42; foreign authentic Function rejected both without callback; output8/minus7, frame and post-construction step/depth cleanup physical0\n",sites);
}
#endif // XIR_CTFE_FUNCTION_CASES_H
