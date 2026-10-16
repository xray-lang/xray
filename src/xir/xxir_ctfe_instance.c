/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_instance.c - Definition-preserving closed compiler result ownership
 *
 * KEY CONCEPT:
 *   Retained immutable inputs preserve lifetimes, never admission. Every factory
 *   receives fresh Checked bytes and executes under its original finite ledger.
 */
#include "xxir_ctfe_instance_internal.h"
#include "xxir_internal.h"
#include "xxir_checked.h"
#include "xxir_compile_memory.h"
#include <string.h>

struct XirCtfeInstanceResult {
    XrXirCompileContext context;
    XrXirArtifact *checked,*lowered;
    XrXirValue value;
    uint32_t function;
};

static XrXirCtfeStatus instance_compile_status(XrXirStatus status) {
    return status==XR_XIR_OK?XR_XIR_CTFE_OK:status==XR_XIR_OUT_OF_MEMORY?XR_XIR_CTFE_OUT_OF_MEMORY:
        status==XR_XIR_BUDGET?XR_XIR_CTFE_BUDGET:status==XR_XIR_BAD_LAYOUT?XR_XIR_CTFE_PROFILE:XR_XIR_CTFE_BAD_ARTIFACT;
}
static bool instance_limits_valid(const XirCtfeInstanceRequest *request) {
    return request->limits.steps<=XR_XIR_CTFE_MAX_STEPS &&
        request->limits.frame_bytes<=XR_XIR_CTFE_MAX_FRAME_BYTES &&
        request->limits.output_bytes<=XR_XIR_CTFE_MAX_OUTPUT_BYTES &&
        request->call_depth<=XR_XIR_CTFE_MAX_CALL_DEPTH;
}
/* Full artifact admission precedes this physical correspondence observation.
 * Concrete equality never repairs missing symbolic constraints or witnesses. */
static XrXirCtfeStatus instance_definition(const XrXirArtifact *artifact,uint32_t function,
    const XrXirArtifact **definition,uint32_t *declaration) {
    if (!xir_compile_work(&artifact->context,sizeof(XrXirOrigin)+8)) return XR_XIR_CTFE_BUDGET;
    const XrXirModule *module=&artifact->module;const XrXirProvenance *proof=module->provenance;
    if (module->stage!=XR_XIR_CHECKED || module->generics || !module->declarations ||
        function>=module->function_count ||
        !proof || proof->kind!=XR_XIR_EVIDENCE_INSTANCE || !proof->source ||
        !proof->origins || proof->count!=module->function_count) return XR_XIR_CTFE_BAD_ARTIFACT;
    const XrXirOrigin *origin=&proof->origins[function];const XrXirModule *source=&proof->source->module;
    if (module->functions[function].parameter_count || !origin->argument_count || !origin->arguments || !source->generics ||
        origin->function>=source->function_count ||
        source->generics[origin->function].parameter_count!=origin->argument_count ||
        source->functions[origin->function].parameter_count) return XR_XIR_CTFE_NOT_SUPPORTED;
    *definition=proof->source;*declaration=origin->function;return XR_XIR_CTFE_OK;
}
static XrXirStatus instance_inputs_match(const XirCtfeInstanceResult *owner) {
    if (owner->checked->context.resources!=owner->context.resources ||
        owner->lowered->context.resources!=owner->context.resources) return XR_XIR_BAD_STRUCTURE;
    XrXirCheckedPacket current={0};XrXirStatus status=xr_xir_compile_checked_write(owner->checked,&current,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(owner->lowered,NULL);
    if (status==XR_XIR_OK) {
        const XrXirCheckedPacket *original=&owner->lowered->checked_packet;
        if (!xir_compile_work(&owner->context,(uint64_t)current.length+2*sizeof(*original))) status=XR_XIR_BUDGET;
        else if (current.length!=original->length || !original->bytes ||
            memcmp(current.bytes,original->bytes,current.length)) status=XR_XIR_BAD_STRUCTURE;
    }
    xr_xir_compile_checked_packet_free(&current);return status;
}
XR_FUNC void xr_xir_compile_ctfe_instance_free(XirCtfeInstanceResult *owner) {
    if (!owner) return;
    xr_xir_value_drop(&owner->value);
    xr_xir_compile_artifact_free(owner->lowered);
    xr_xir_compile_artifact_free(owner->checked);
    xr_compile_resources_free(owner);
}
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_instance(const XirCtfeInstanceRequest *request,
    XirCtfeInstanceResult **output) {
    if (!request || !request->checked || !output || *output || !instance_limits_valid(request))
        return XR_XIR_CTFE_BAD_ARGUMENT;
    const XrXirArtifact *input=request->checked;
    if (!xir_compile_context_valid(&input->context)) return XR_XIR_CTFE_BAD_ARTIFACT;
    XrXirStatus status=xr_xir_compile_artifact_verify(input,NULL);
    if (status!=XR_XIR_OK) return instance_compile_status(status);
    const XrXirArtifact *definition=NULL;uint32_t declaration=0;
    XrXirCtfeStatus admitted=instance_definition(input,request->function,&definition,&declaration);
    if (admitted!=XR_XIR_CTFE_OK) return admitted;
    (void)definition;(void)declaration;
    XirCtfeInstanceResult *owner=xir_compile_calloc(&input->context,1,sizeof(*owner),&status);
    if (!owner) return instance_compile_status(status);
    if (!xir_compile_work(&input->context,sizeof(owner->context)+sizeof(owner->function))) {
        xr_xir_compile_ctfe_instance_free(owner);return XR_XIR_CTFE_BUDGET;
    }
    owner->context=input->context;owner->function=request->function;
    XrXirCheckedPacket packet={0};status=xr_xir_compile_checked_write(input,&packet,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(&owner->context,packet.bytes,packet.length,&owner->checked,NULL);
    xr_xir_compile_checked_packet_free(&packet);
    if (status==XR_XIR_OK) status=xr_xir_compile_lower(owner->checked,&request->target,&owner->lowered,NULL);
    if (status!=XR_XIR_OK) { xr_xir_compile_ctfe_instance_free(owner);return instance_compile_status(status); }
    XrXirCtfeCallRequest call={.evaluation={.artifact=owner->lowered,.function=request->function,
        .target=request->target,.limits=request->limits},.call_depth=request->call_depth};
    if (!xir_compile_work(&owner->context,sizeof(call)+sizeof(call.evaluation.checked_identity))) {
        xr_xir_compile_ctfe_instance_free(owner);return XR_XIR_CTFE_BUDGET;
    }
    memcpy(call.evaluation.checked_identity,owner->lowered->checked_identity,sizeof(call.evaluation.checked_identity));
    admitted=xr_xir_compile_ctfe_calls(&call,&owner->value);
    if (admitted!=XR_XIR_CTFE_OK) { xr_xir_compile_ctfe_instance_free(owner);return admitted; }
    if (!xir_compile_work(&owner->context,sizeof(*output))) {
        xr_xir_compile_ctfe_instance_free(owner);return XR_XIR_CTFE_BUDGET;
    }
    *output=owner;return XR_XIR_CTFE_OK;
}
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_instance_view(const XirCtfeInstanceResult *owner,
    XirCtfeInstanceView *output) {
    if (!owner || !output || output->checked || output->lowered || output->definition || output->value ||
        output->instance_function || output->definition_function) return XR_XIR_CTFE_BAD_ARGUMENT;
    if (!xir_compile_work(&owner->context,sizeof(*output))) return XR_XIR_CTFE_BUDGET;
    XrXirStatus status=instance_inputs_match(owner);
    if (status!=XR_XIR_OK) return instance_compile_status(status);
    const XrXirArtifact *definition=NULL;uint32_t declaration=0;
    XrXirCtfeStatus admitted=instance_definition(owner->checked,owner->function,&definition,&declaration);
    if (admitted!=XR_XIR_CTFE_OK) return admitted;
    if (owner->lowered->context.resources!=owner->context.resources ||
        owner->lowered->module.functions[owner->function].result!=(XrXirType)owner->value.type)
        return XR_XIR_CTFE_BAD_ARTIFACT;
    *output=(XirCtfeInstanceView){owner->checked,owner->lowered,definition,&owner->value,owner->function,declaration};
    return XR_XIR_CTFE_OK;
}
