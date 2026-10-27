/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_comptime_resolve.inc.c - Atomic Source receiving without syntax producers
 *
 * KEY CONCEPT:
 *   Ordinary specialization chooses actual missing keys. The sole evaluator
 *   and ordinary literal writer supply providers before complete rechecking.
 */
#include "xxir_comptime_resolve_internal.h"
#include "xxir_comptime_pending_internal.h"
static XrXirStatus resolution_status(XrXirDiagnostic *diagnostic,XrXirStatus status) {
    if (diagnostic) diagnostic->status=status;
    return status;
}
static XrXirStatus resolution_ctfe_status(XrXirCtfeStatus status) {
    if (status==XR_XIR_CTFE_OK) return XR_XIR_OK;
    if (status==XR_XIR_CTFE_OUT_OF_MEMORY) return XR_XIR_OUT_OF_MEMORY;
    if (status==XR_XIR_CTFE_MEMORY_LIMIT) return XR_XIR_MEMORY_LIMIT;
    if (status==XR_XIR_CTFE_OVERFLOW) return XR_XIR_OVERFLOW;
    if (status==XR_XIR_CTFE_CANCELLED) return XR_XIR_CANCELLED;
    if (status==XR_XIR_CTFE_DEADLINE) return XR_XIR_DEADLINE;
    if (status==XR_XIR_CTFE_CLOCK_FAILURE) return XR_XIR_CLOCK_FAILURE;
    if (status==XR_XIR_CTFE_COMPLEXITY_LIMIT || status==XR_XIR_CTFE_STEP_LIMIT || status==XR_XIR_CTFE_FRAME_LIMIT ||
        status==XR_XIR_CTFE_OUTPUT_LIMIT || status==XR_XIR_CTFE_DEPTH_LIMIT) return XR_XIR_BUDGET;
    if (status==XR_XIR_CTFE_PROFILE) return XR_XIR_BAD_LAYOUT;
    if (status==XR_XIR_CTFE_BAD_ARGUMENT || status==XR_XIR_CTFE_BAD_ARTIFACT || status==XR_XIR_CTFE_IDENTITY)
        return XR_XIR_BAD_STRUCTURE;
    return XR_XIR_BAD_TYPE;
}
static XrXirStatus resolution_bind(const XirLiteralDefinitions *literal,const XrXirComptimeSite *wanted,
    XrXirArtifact **output,XrXirDiagnostic *diagnostic) {
    if (literal->count!=1 || !literal->definition || !literal->bindings) return XR_XIR_BAD_STRUCTURE;
    const XrXirArtifact *definition=literal->definition;const XirLiteralBinding *provider=&literal->bindings[0];
    if (!provider->argument_count || !provider->arguments || provider->definition!=wanted->helper) return XR_XIR_BAD_STRUCTURE;
    XirComptimeSelection selection={&definition->module,definition->module.types,wanted->caller,wanted->instruction,
        provider->arguments,provider->argument_count};
    const XrXirComptimeBinding *binding=NULL;
    XrXirStatus status=xir_comptime_select(&definition->context,definition->construction,&selection,&binding);
    if (status==XR_XIR_OK && (!binding || binding->provider!=provider->provider)) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK) status=xr_xir_compile_recheck_v2(&definition->context,&definition->module,
        definition->construction,output,diagnostic);
    return status;
}

static XrXirStatus resolution_execute(const XirComptimeResolveRequest *request,const XirComptimePending *pending,
    XrXirArtifact **output,XirComptimeResolveFailure *failure,XrXirDiagnostic *diagnostic,SourceContext *source_owner) {
    XrXirArtifact *checked=NULL;XirCtfeInstanceBatch *batch=NULL;XirLiteralDefinitions literal={0};
    XrXirStatus status=xr_xir_compile_comptime_pending_specialize(pending,&checked,diagnostic);
    XrXirComptimeSite site={0};
    if (status==XR_XIR_OK) {
        const XrXirProvenance *proof=checked->module.provenance;const XirEvaluationRoot *root=&proof->evaluation;
        site=(XrXirComptimeSite){proof->origins[root->caller].function,root->instruction,proof->origins[root->callee].function};
        XirCtfeInstanceBatchRequest execute={checked,&root->callee,1,request->target,request->limits,request->call_depth};
        uint32_t failed=UINT32_MAX;XrXirCtfeStatus evaluated=xr_xir_compile_ctfe_instance_batch_at(&execute,request->control,&batch,&failed);
        status=resolution_ctfe_status(evaluated);
        if (status!=XR_XIR_OK && failure) {
            failure->status=evaluated;failure->root_local=failed==0;
            if (failure->root_local) failure->site=site;
        }
        if (status!=XR_XIR_OK && failed==0 && diagnostic)
            *diagnostic=(XrXirDiagnostic){status,site.caller,UINT32_MAX,site.instruction,XR_XIR_DIAGNOSTIC_NONE};
    }
    if (status==XR_XIR_OK) {
        XirLiteralRequest receiving={&checked->context,batch,1,request->control};
        status=literal_receive(&receiving,&site,&literal,diagnostic,source_owner);
    }
    if (status==XR_XIR_OK) status=resolution_bind(&literal,&site,output,diagnostic);
    xr_xir_compile_instance_literals_free(&literal);xr_xir_compile_ctfe_instance_batch_free(batch);
    xr_xir_compile_artifact_free(checked);return status;
}
static XrXirStatus source_comptime_resolve(const XirComptimeResolveRequest *request,
    XrXirArtifact **output,XirComptimeResolveFailure *failure,XrXirDiagnostic *diagnostic,SourceContext *source_owner) {
    if (!request || !request->definition || !output || *output ||
        !xir_compile_context_valid(&request->definition->context) ||
        (failure && (failure->status || failure->root_local || failure->site.caller || failure->site.instruction || failure->site.helper)) ||
        request->limits.steps>XR_XIR_CTFE_MAX_STEPS || request->limits.frame_bytes>XR_XIR_CTFE_MAX_FRAME_BYTES ||
        request->limits.output_bytes>XR_XIR_CTFE_MAX_OUTPUT_BYTES || request->call_depth>XR_XIR_CTFE_MAX_CALL_DEPTH)
        return resolution_status(diagnostic,XR_XIR_BAD_STRUCTURE);
    const XrXirArtifact *input=request->definition;
    if (input->module.stage!=XR_XIR_CHECKED || input->module.linkage_kind!=XR_XIR_PROGRAM ||
        xir_effect_evidence_is_closed(&input->module) || xir_artifact_is_evaluation(input))
        return resolution_status(diagnostic,XR_XIR_BAD_STAGE);
    if (request->target.architecture!=XR_XIR_ARCH_X86_64 || request->target.abi_version!=XR_XIR_VALUE_ABI_VERSION)
        return resolution_status(diagnostic,XR_XIR_BAD_LAYOUT);
    XrXirCheckedPacket packet={0};XrXirArtifact *current=NULL;
    XrXirStatus status=xr_xir_compile_checked_write(input,&packet,diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(&input->context,packet.bytes,packet.length,&current,diagnostic);
    xr_xir_compile_checked_packet_free(&packet);
    while (status==XR_XIR_OK) {
        XirComptimePending *pending=NULL;XrXirArtifact *next=NULL;
        status=xr_xir_compile_comptime_pending(current,&pending,diagnostic);
        if (status==XR_XIR_OK && !pending) break;
        if (status==XR_XIR_OK) status=resolution_execute(request,pending,&next,failure,diagnostic,source_owner);
        xr_xir_compile_comptime_pending_free(pending);
        if (status==XR_XIR_OK) { xr_xir_compile_artifact_free(current);current=next; }
        else xr_xir_compile_artifact_free(next);
    }

    if (status==XR_XIR_OK) { *output=current;current=NULL; }
    xr_xir_compile_artifact_free(current);return resolution_status(diagnostic,status);
}
XR_FUNC XrXirStatus xr_xir_compile_comptime_resolve(const XirComptimeResolveRequest *request,
    XrXirArtifact **output,XirComptimeResolveFailure *failure,XrXirDiagnostic *diagnostic) {
    return source_comptime_resolve(request,output,failure,diagnostic,NULL);
}
