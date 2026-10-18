/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_receiving.c - Complete current Source checks in Instance receiving
 *
 * KEY CONCEPT:
 *   Retained storage cannot hide changed bodies, contracts or layouts. Every
 *   physical failure and retry still belongs to its original finite owner.
 */
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1); } } while (0)
#include "xir_source_compile_owner.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_ctfe.h"

static const XrCompileResourceLimits caps={UINT64_C(67108864),UINT64_C(8388608),UINT64_C(128000000)};
static const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
typedef struct ReceivingJob { XrXirCompileContext context;XrXirArtifact *artifact;size_t blocks,bytes; } ReceivingJob;
static XrCompileResourceStats stats(const XrXirCompileContext *context) {
    XrCompileResourceStats result={0};CHECK(xr_compile_resources_stats(context->resources,&result)==XR_COMPILE_RESOURCE_OK);return result;
}
static void fixture(const XrXirCompileContext *context,XrXirCheckedPacket *wire) {
    static const char text[]="fn plus(x:i64)->i64{return x+2}\nexport fn run()->i64{return plus(40)}\n";
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"source-receiving",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,text,sizeof(text)-1};XrXirSourceResult source={0};XrXirArtifact *closed=NULL;
    CHECK(xr_xir_compile_source_check_text(&request,&input,&source,NULL,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(source.checked,&closed,NULL)==XR_XIR_OK);
    CHECK(closed->module.provenance && closed->module.provenance->kind==XR_XIR_EVIDENCE_INSTANCE);
    CHECK(xr_xir_compile_checked_write(closed,wire,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_source_result_free(&source);xr_compile_session_free(session);
}
static XrXirStatus job_read(ReceivingJob *job,const XrXirCheckedPacket *wire,bool lowered) {
    return lowered?xr_xir_compile_checked_read_lower(&job->context,wire->bytes,wire->length,&target,&job->artifact,NULL):
        xr_xir_compile_checked_read(&job->context,wire->bytes,wire->length,&job->artifact,NULL);
}
static void job_new(ReceivingJob *job,const XrCompileResourceLimits *limits) {
    *job=(ReceivingJob){0};job->blocks=source_fixture_compile_live;job->bytes=source_fixture_compile_bytes;
    CHECK(xr_compile_resources_new(limits,&job->context.resources)==XR_COMPILE_RESOURCE_OK);
    job->context.limits=xr_xir_compile_default_limits();
}
static void job_free(ReceivingJob *job) {
    xr_xir_compile_artifact_free(job->artifact);xr_compile_resources_release(job->context.resources);
    CHECK(source_fixture_compile_live==job->blocks && source_fixture_compile_bytes==job->bytes);
}
static uint32_t run_function(const XrXirArtifact *artifact) {
    uint32_t found=UINT32_MAX;
    for (uint32_t f=0;f<artifact->module.function_count;++f) {
        const XrXirFunction *body=&artifact->module.functions[f];
        if (body->name_length==3 && !memcmp(body->name,"run",3)) found=f;
    }
    CHECK(found!=UINT32_MAX);return found;
}
static void value42(const XrXirArtifact *artifact) {
    XrXirCtfeCallRequest request={0};request.evaluation.artifact=artifact;request.evaluation.function=run_function(artifact);
    request.evaluation.target=target;request.evaluation.limits=(XrXirCtfeLimits){XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES};
    memcpy(request.evaluation.checked_identity,artifact->checked_identity,32);request.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH;
    XrXirValue output={0};CHECK(xr_xir_compile_ctfe_calls(&request,&output)==XR_XIR_CTFE_OK);
    CHECK(output.type==XR_XIR_I64 && output.payload==42);xr_xir_value_drop(&output);
}
static void refuse(XrXirArtifact *artifact,bool lowered) {
    XrCompileResourceStats before=stats(&artifact->context);XrXirStatus status=xr_xir_compile_artifact_verify(artifact,NULL);
    CHECK(status!=XR_XIR_OK && status!=XR_XIR_BUDGET && status!=XR_XIR_OUT_OF_MEMORY);
    CHECK(stats(&artifact->context).live_bytes==before.live_bytes);
    if (lowered) {
        XrXirCtfeCallRequest request={0};request.evaluation.artifact=artifact;request.evaluation.target=target;
        request.evaluation.function=run_function(artifact);
        request.evaluation.limits=(XrXirCtfeLimits){XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES};
        memcpy(request.evaluation.checked_identity,artifact->checked_identity,32);request.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH;
        XrXirValue output={0};CHECK(xr_xir_compile_ctfe_calls(&request,&output)==XR_XIR_CTFE_BAD_ARTIFACT && !output.type && !output.payload);
    } else {
        XrXirCheckedPacket output={0};CHECK(xr_xir_compile_checked_write(artifact,&output,NULL)!=XR_XIR_OK && !output.bytes && !output.length);
    }
    CHECK(stats(&artifact->context).live_bytes==before.live_bytes);
}
static void mutations(const XrXirCheckedPacket *wire,bool lowered) {
    ReceivingJob job;job_new(&job,&caps);CHECK(job_read(&job,wire,lowered)==XR_XIR_OK);
    XrXirArtifact *source=job.artifact->module.provenance->source;CHECK(source && source->source_facts);
    XrXirInstruction *constant=NULL;XrXirFunction *parameter=NULL;
    for (uint32_t f=0;f<source->module.function_count;++f) {
        XrXirFunction *body=(XrXirFunction *)&source->module.functions[f];if (body->parameter_count) parameter=body;
        for (uint32_t i=0;i<body->instruction_count;++i)
            if (body->instructions[i].op==XR_XIR_CONST_INT && body->instructions[i].immediate==2) constant=(XrXirInstruction *)&body->instructions[i];
    }
    CHECK(constant && parameter);int64_t number=constant->immediate;constant->immediate=3;refuse(job.artifact,lowered);constant->immediate=number;
    XrXirType *type=(XrXirType *)&parameter->parameters[0],held_type=*type;*type=XR_XIR_STRING;refuse(job.artifact,lowered);*type=held_type;
    XrXirFunctionEffectContract *contract=(XrXirFunctionEffectContract *)&source->module.provenance->contracts[0];
    uint32_t mask=contract->formula.constant_mask;contract->formula.constant_mask^=XR_XIR_CALLABLE_ROOT_REQUIRED;refuse(job.artifact,lowered);contract->formula.constant_mask=mask;
    XrXirTarget held=source->target;source->target.abi_version=XR_XIR_VALUE_ABI_VERSION;refuse(job.artifact,lowered);source->target=held;
    CHECK(xr_xir_compile_artifact_verify(job.artifact,NULL)==XR_XIR_OK);if (lowered) value42(job.artifact);
    job_free(&job);fprintf(stderr,"Source receiving stage=%u body/type/contract/layout tampering refused; restored full check and42 physical0\n",lowered?1u:0u);
}
static void fault(const XrXirCheckedPacket *wire,bool lowered,size_t ordinal,size_t sites) {
    ReceivingJob job;job_new(&job,&caps);CHECK(job_read(&job,wire,lowered)==XR_XIR_OK);
    XrCompileResourceStats before=stats(&job.context);XrCompileResources *same=job.context.resources;
    size_t start=source_fixture_compile_attempts;source_fixture_compile_fail_at=start+ordinal;source_fixture_compile_injected=false;
    CHECK(xr_xir_compile_artifact_verify(job.artifact,NULL)==XR_XIR_OUT_OF_MEMORY);
    CHECK(source_fixture_compile_injected && source_fixture_compile_attempts==start+ordinal+1);
    XrCompileResourceStats failed=stats(&job.context);CHECK(failed.live_bytes==before.live_bytes);source_fixture_compile_fail_at=SIZE_MAX;
    start=source_fixture_compile_attempts;CHECK(xr_xir_compile_artifact_verify(job.artifact,NULL)==XR_XIR_OK);
    CHECK(source_fixture_compile_attempts==start+sites && job.context.resources==same);
    XrCompileResourceStats after=stats(&job.context);CHECK(after.live_bytes==before.live_bytes && after.work>=failed.work && after.allocated_bytes>=failed.allocated_bytes);
    CHECK(after.work<=caps.work && after.allocated_bytes<=caps.allocated_bytes && after.peak_bytes<=caps.live_bytes);job_free(&job);
}
static XrCompileResourceStats pipeline(const XrXirCheckedPacket *wire,bool lowered,const XrCompileResourceLimits *limits,bool short_budget) {
    ReceivingJob job;job_new(&job,limits);XrXirStatus status=job_read(&job,wire,lowered);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(job.artifact,NULL);
    CHECK(status==(short_budget?XR_XIR_BUDGET:XR_XIR_OK));XrCompileResourceStats spent=stats(&job.context);
    if (short_budget) {
        if (job.artifact) CHECK(xr_xir_compile_artifact_verify(job.artifact,NULL)==XR_XIR_BUDGET);
        else CHECK(job_read(&job,wire,lowered)==XR_XIR_BUDGET && !job.artifact);
        CHECK(stats(&job.context).work>=spent.work && stats(&job.context).allocated_bytes>=spent.allocated_bytes);
    }
    job_free(&job);return spent;
}
static void resources(const XrXirCheckedPacket *wire,bool lowered) {
    ReceivingJob job;job_new(&job,&caps);CHECK(job_read(&job,wire,lowered)==XR_XIR_OK);
    size_t start=source_fixture_compile_attempts;XrCompileResourceStats before=stats(&job.context);
    CHECK(xr_xir_compile_artifact_verify(job.artifact,NULL)==XR_XIR_OK);
    size_t sites=source_fixture_compile_attempts-start;CHECK(sites && stats(&job.context).live_bytes==before.live_bytes);job_free(&job);
    for (size_t ordinal=0;ordinal<sites;++ordinal) fault(wire,lowered,ordinal,sites);
    XrCompileResourceStats complete=pipeline(wire,lowered,&caps,false);
    for (unsigned axis=0;axis<3;++axis) {
        XrCompileResourceLimits exact=caps;
        if (!axis) exact.work=complete.work;else if (axis==1) exact.allocated_bytes=complete.allocated_bytes;else exact.live_bytes=complete.peak_bytes;
        pipeline(wire,lowered,&exact,false);
        if (!axis) --exact.work;else if (axis==1) --exact.allocated_bytes;else --exact.live_bytes;
        pipeline(wire,lowered,&exact,true);
    }
    fprintf(stderr,"Source receiving stage=%u full FI=%zu first-error/same-owner retry; complete pipeline three axes exact/minus1; physical0\n",lowered?1u:0u,sites);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--resources")));
    ReceivingJob owner;job_new(&owner,&caps);XrXirCheckedPacket wire={0};fixture(&owner.context,&wire);
    if (argc==1) { mutations(&wire,false);mutations(&wire,true); }
    else { resources(&wire,false);resources(&wire,true); }
    xr_xir_compile_checked_packet_free(&wire);job_free(&owner);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);return 0;
}
