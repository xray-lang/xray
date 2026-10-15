/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ctfe_dynamic_loan_cases.h - Growing data loans on authentic Class roots
 */
#ifndef XIR_CTFE_DYNAMIC_LOAN_CASES_H
#define XIR_CTFE_DYNAMIC_LOAN_CASES_H
#include "xir/xxir_internal.h"
typedef struct ReferenceDynamicJob {
    XrXirCompileContext context;
    XrXirArtifact *lowered;
    XrXirCtfeRequest request;
    XrCompileResourceStats baseline;
    size_t blocks,bytes;
} ReferenceDynamicJob;
typedef struct ReferenceDynamicLimits { uint64_t steps,frame,output;uint32_t depth; } ReferenceDynamicLimits;
static void reference_dynamic_value(const XrXirValue *output,bool owning) {
    if (owning) reference_class_string(output);
    else CHECK(output->type==XR_XIR_I64 && output->payload==42);
}
static void reference_dynamic_job_new(ReferenceDynamicJob *job,const XrXirCheckedPacket *wire,const uint8_t *identity) {
    *job=(ReferenceDynamicJob){0};job->blocks=source_fixture_compile_live;job->bytes=source_fixture_compile_bytes;
    CHECK(xr_compile_resources_new(&reference_limits,&job->context.resources)==XR_COMPILE_RESOURCE_OK);
    job->context.limits=xr_xir_compile_default_limits();
    uint8_t digest[32]={0};CHECK(xr_compile_resources_work(job->context.resources,wire->length)==XR_COMPILE_RESOURCE_OK);
    xr_sha256(wire->bytes,wire->length,digest);CHECK(!memcmp(digest,identity,32));
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    /* This existing byte consumer fully decodes/checks an exclusive input and
     * immediately lowers it with complete receiving checks. No artifact or
     * retained proof enters the factory. Both evaluator calls below still
     * perform their own complete verification on this original finite owner. */
    CHECK(xr_xir_compile_checked_read_lower(&job->context,wire->bytes,wire->length,&target,&job->lowered,NULL)==XR_XIR_OK);
    job->request=(XrXirCtfeRequest){.artifact=job->lowered,
        .function=reference_run_function(xr_xir_compile_artifact_module(job->lowered)),.target=target,
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}};
    memcpy(job->request.checked_identity,identity,32);job->baseline=reference_stats(&job->context);
}
static void reference_dynamic_job_free(ReferenceDynamicJob *job) {
    CHECK(reference_stats(&job->context).live_bytes==job->baseline.live_bytes);
    xr_xir_compile_artifact_free(job->lowered);xr_compile_resources_release(job->context.resources);
    CHECK(source_fixture_compile_live==job->blocks && source_fixture_compile_bytes==job->bytes);
}
static void reference_dynamic_fault(const XrXirCheckedPacket *wire,const uint8_t *identity,
    size_t fault,size_t sites,bool owning) {
    ReferenceDynamicJob job;reference_dynamic_job_new(&job,wire,identity);XrXirValue output={0};
    XrCompileResources *same=job.context.resources;size_t start=source_fixture_compile_attempts;
    source_fixture_compile_injected=false;source_fixture_compile_fail_at=start+fault;
    CHECK(reference_text_evaluate(&job.request,true,&output)==XR_XIR_CTFE_OUT_OF_MEMORY);
    CHECK(source_fixture_compile_injected && source_fixture_compile_attempts==start+fault+1 && !output.type && !output.payload);
    CHECK(reference_stats(&job.context).live_bytes==job.baseline.live_bytes);source_fixture_compile_fail_at=SIZE_MAX;
    XrCompileResourceStats failed=reference_stats(&job.context);start=source_fixture_compile_attempts;
    CHECK(reference_text_evaluate(&job.request,true,&output)==XR_XIR_CTFE_OK);reference_dynamic_value(&output,owning);
    CHECK(source_fixture_compile_attempts-start==sites);xr_xir_value_drop(&output);
    XrCompileResourceStats after=reference_stats(&job.context);
    CHECK(job.context.resources==same && after.work>=failed.work && after.allocated_bytes>=failed.allocated_bytes);
    CHECK(after.work<=reference_limits.work && after.allocated_bytes<=reference_limits.allocated_bytes && after.peak_bytes<=reference_limits.live_bytes);
    reference_dynamic_job_free(&job);
}
static XrXirCtfeStatus reference_dynamic_limit(const XrXirCheckedPacket *wire,const uint8_t *identity,
    ReferenceDynamicLimits limits,bool owning) {
    ReferenceDynamicJob job;reference_dynamic_job_new(&job,wire,identity);XrXirValue output={0};
    job.request.limits=(XrXirCtfeLimits){limits.steps,limits.frame,limits.output};
    XrXirCtfeCallRequest request={job.request,limits.depth};
    XrXirCtfeStatus status=xr_xir_compile_ctfe_calls(&request,&output);
    if (status==XR_XIR_CTFE_OK) { reference_dynamic_value(&output,owning);xr_xir_value_drop(&output); }
    else CHECK(!output.type && !output.payload);
    reference_dynamic_job_free(&job);return status;
}
static void reference_dynamic_source_string(const XrXirCompileContext *context,XrXirSourceResult *source) {
    const XrXirModule *module=xr_xir_compile_artifact_module(source->checked);
    const XrXirFunction *body=&module->functions[reference_run_function(module)];bool literal=false;
    for (uint32_t i=0;i<body->instruction_count;++i) {
        const XrXirInstruction *op=&body->instructions[i];
        CHECK(op->op!=XR_XIR_CALL && op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_FUNCTION_REF);
        if (op->op==XR_XIR_CONST_STRING) {
            const XrXirLiteral *text=&module->declarations->literals[op->immediate];
            if (text->length==2 && !memcmp(text->bytes,"ab",2)) literal=true;
        }
    }
    CHECK(literal);XrXirArtifact *closed=NULL;XrXirCheckedPacket wire={0};
    CHECK(xr_xir_compile_specialize(source->checked,&closed,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(closed,&wire,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);XrXirValue output={0};
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_run_function(xr_xir_compile_artifact_module(lowered)),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}};
    memcpy(request.checked_identity,identity,32);XrCompileResourceStats baseline=reference_stats(context);
    size_t start=source_fixture_compile_attempts;CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK);
    size_t sites=source_fixture_compile_attempts-start;CHECK(sites);reference_class_string(&output);xr_xir_value_drop(&output);
    for (size_t fault=0;fault<sites;++fault) reference_dynamic_fault(&wire,identity,fault,sites,true);
    request.limits.output_bytes=1;CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OUTPUT_LIMIT && !output.type && !output.payload);
    request.limits.output_bytes=2;CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK);reference_class_string(&output);xr_xir_value_drop(&output);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_checked_packet_free(&wire);
    fprintf(stderr,"CTFE dynamic Class loan Source owning String literal ab, complete FI=%zu per-job same-owner retry, exact2/minus1 physical0\n",sites);
}
static void reference_dynamic_direct(const XrXirCompileContext *context,XrXirSourceResult *source,bool owning) {
    XrXirArtifact *closed=NULL;XrXirCheckedPacket wire={0};
    CHECK(xr_xir_compile_specialize(source->checked,&closed,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(closed,&wire,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);XrXirValue output={0};
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t projects=0;
    for (uint32_t f=0;f<module->function_count;++f)
        for (uint32_t i=0;i<module->functions[f].instruction_count;++i)
            if (module->functions[f].instructions[i].op==XR_XIR_CELL_PROJECT) ++projects;
    CHECK(projects);XrXirCtfeRequest request={.artifact=lowered,.function=reference_run_function(module),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}};
    memcpy(request.checked_identity,identity,32);XrCompileResourceStats baseline=reference_stats(context);
    CHECK(reference_text_evaluate(&request,false,&output)==XR_XIR_CTFE_NOT_LEAF && !output.type && !output.payload);
    size_t start=source_fixture_compile_attempts;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK);reference_dynamic_value(&output,owning);
    size_t sites=source_fixture_compile_attempts-start;CHECK(sites);xr_xir_value_drop(&output);
    fprintf(stderr,"CTFE dynamic Class loan census owning=%u sites=%zu work=%llu\n",(unsigned)owning,sites,(unsigned long long)reference_stats(context).work);
    for (size_t fault=0;fault<sites;++fault) {
        reference_dynamic_fault(&wire,identity,fault,sites,owning);
        if (!(fault%32) || fault+1==sites) fprintf(stderr,"CTFE dynamic Class loan complete FI ordinal=%zu/%zu physical0\n",fault+1,sites);
    }
    ReferenceDynamicLimits limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES,XR_XIR_CTFE_MAX_CALL_DEPTH};
    uint64_t steps=0;
    for (;;++steps) {
        CHECK(steps<XR_XIR_CTFE_MAX_STEPS);limits.steps=steps;
        XrXirCtfeStatus status=reference_dynamic_limit(&wire,identity,limits,owning);
        if (status==XR_XIR_CTFE_OK) break;CHECK(status==XR_XIR_CTFE_STEP_LIMIT);
    }
    limits.steps=XR_XIR_CTFE_MAX_STEPS;uint32_t depth=1;
    for (;;++depth) {
        CHECK(depth<=XR_XIR_CTFE_MAX_CALL_DEPTH);limits.depth=depth;
        XrXirCtfeStatus status=reference_dynamic_limit(&wire,identity,limits,owning);
        if (status==XR_XIR_CTFE_OK) break;CHECK(status==XR_XIR_CTFE_DEPTH_LIMIT);
    }
    limits.depth=XR_XIR_CTFE_MAX_CALL_DEPTH;uint64_t low=0,high=XR_XIR_CTFE_MAX_FRAME_BYTES;
    while (low<high) {
        uint64_t middle=low+(high-low)/2;limits.frame=middle;
        XrXirCtfeStatus status=reference_dynamic_limit(&wire,identity,limits,owning);
        if (status==XR_XIR_CTFE_OK) high=middle;
        else { CHECK(status==XR_XIR_CTFE_FRAME_LIMIT || status==XR_XIR_CTFE_BUDGET);low=middle+1; }
    }
    CHECK(low);limits.frame=low;CHECK(reference_dynamic_limit(&wire,identity,limits,owning)==XR_XIR_CTFE_OK);
    limits.frame=low-1;XrXirCtfeStatus short_frame=reference_dynamic_limit(&wire,identity,limits,owning);
    CHECK(short_frame==XR_XIR_CTFE_FRAME_LIMIT || short_frame==XR_XIR_CTFE_BUDGET);
    limits.frame=XR_XIR_CTFE_MAX_FRAME_BYTES;limits.output=owning?2:8;
    CHECK(reference_dynamic_limit(&wire,identity,limits,owning)==XR_XIR_CTFE_OK);
    --limits.output;CHECK(reference_dynamic_limit(&wire,identity,limits,owning)==XR_XIR_CTFE_OUTPUT_LIMIT);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);uint32_t completed=1;
    for (;;) {
        XrXirCtfeStatus status=reference_text_evaluate(&request,true,&output);
        if (status!=XR_XIR_CTFE_OK) { CHECK(status==XR_XIR_CTFE_BUDGET && !output.type && !output.payload);break; }
        reference_dynamic_value(&output,owning);xr_xir_value_drop(&output);++completed;
    }
    uint64_t spent=reference_stats(context).work;
    CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_BUDGET && !output.type && !output.payload);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes && spent<=reference_limits.work);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_checked_packet_free(&wire);
    fprintf(stderr,"CTFE dynamic Class loan owning=%u FI=%zu complete per-job same-owner retry/live; all steps0..%llu depth1..%u frame exact%llu/minus-one status%u output exact%u/minus-one; original owner work%llu/128000000 after%u successes then retry BUDGET physical0\n",
        (unsigned)owning,sites,(unsigned long long)steps,depth,(unsigned long long)low,(unsigned)short_frame,owning?2:8,(unsigned long long)spent,completed);
}
static void reference_dynamic_last_owner(const char *name) {
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&reference_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(reference_source(&context,name,&source,&diagnostic)==XR_XIR_OK);
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(&context,&source,identity);XrXirValue output={0};
    XrXirCtfeRequest request={.artifact=lowered,.function=reference_run_function(xr_xir_compile_artifact_module(lowered)),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,2}};
    memcpy(request.checked_identity,identity,32);CHECK(reference_text_evaluate(&request,true,&output)==XR_XIR_CTFE_OK);
    xr_xir_compile_artifact_free(lowered);xr_compile_resources_release(context.resources);reference_class_string(&output);
    size_t attempts=source_fixture_compile_attempts;source_fixture_compile_fail_at=attempts;xr_xir_value_drop(&output);
    source_fixture_compile_fail_at=SIZE_MAX;
    CHECK(source_fixture_compile_attempts==attempts && !source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"CTFE dynamic Class loan String survives Source/Artifact/ledger caller death; final drop allocates nothing physical0\n");
}
#endif // XIR_CTFE_DYNAMIC_LOAN_CASES_H
