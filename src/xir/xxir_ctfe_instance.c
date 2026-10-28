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
 *   receives fresh Checked bytes and executes under its original memory owner.
 */
#include "xxir_ctfe_instance_internal.h"
#include "xxir_ctfe_batch_internal.h"
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
        status==XR_XIR_BUDGET?XR_XIR_CTFE_COMPLEXITY_LIMIT:
        status==XR_XIR_MEMORY_LIMIT?XR_XIR_CTFE_MEMORY_LIMIT:status==XR_XIR_OVERFLOW?XR_XIR_CTFE_OVERFLOW:
        status==XR_XIR_CANCELLED?XR_XIR_CTFE_CANCELLED:status==XR_XIR_DEADLINE?XR_XIR_CTFE_DEADLINE:
        status==XR_XIR_CLOCK_FAILURE?XR_XIR_CTFE_CLOCK_FAILURE:status==XR_XIR_BAD_LAYOUT?XR_XIR_CTFE_PROFILE:XR_XIR_CTFE_BAD_ARTIFACT;
}
static XrXirCtfeStatus instance_control_poll(XrCompileRequestControl *control) {
    return control?instance_compile_status(xir_compile_resource_status(xr_compile_request_control_poll(control))):XR_XIR_CTFE_OK;
}
static bool instance_limits_valid(const XirCtfeInstanceRequest *request) {
    return request->limits.steps<=XR_XIR_CTFE_MAX_STEPS &&
        request->limits.frame_bytes<=XR_XIR_CTFE_MAX_FRAME_BYTES &&
        request->limits.output_bytes<=XR_XIR_CTFE_MAX_OUTPUT_BYTES &&
        request->call_depth<=XR_XIR_CTFE_MAX_CALL_DEPTH;
}
static XrXirStatus instance_packet_write(const XrXirArtifact *artifact,XrXirCheckedPacket *packet) {
    return artifact->evaluation_only?xir_evaluation_write(artifact,packet,NULL):
        xr_xir_compile_checked_write(artifact,packet,NULL);
}
static XrXirStatus instance_packet_read(const XrXirCompileContext *context,const XrXirArtifact *input,
    const XrXirCheckedPacket *packet,XrXirArtifact **output) {
    return input->evaluation_only?xir_evaluation_read(context,packet->bytes,packet->length,output,NULL):
        xr_xir_compile_checked_read(context,packet->bytes,packet->length,output,NULL);
}
/* Full artifact admission precedes this physical correspondence observation.
 * Concrete equality never repairs missing symbolic constraints or witnesses. */
static XrXirCtfeStatus instance_definition(const XrXirArtifact *artifact,uint32_t function,
    const XrXirArtifact **definition,uint32_t *declaration) {

    const XrXirModule *module=&artifact->module;const XrXirProvenance *proof=module->provenance;
    if (artifact->evaluation_only && (!proof || function!=proof->evaluation.callee)) return XR_XIR_CTFE_NOT_SUPPORTED;
    if (module->stage!=XR_XIR_CHECKED || module->generics || !module->declarations ||
        function>=module->function_count ||
        !proof || !xir_effect_kind_is_closed(proof->kind) || !proof->source ||
        !proof->origins || proof->count!=module->function_count) return XR_XIR_CTFE_BAD_ARTIFACT;
    const XrXirOrigin *origin=&proof->origins[function];const XrXirModule *source=&proof->source->module;
    if (module->functions[function].parameter_count || !origin->argument_count || !origin->arguments || !source->generics ||
        origin->function>=source->function_count ||
        source->generics[origin->function].parameter_count!=origin->argument_count ||
        source->functions[origin->function].parameter_count) return XR_XIR_CTFE_NOT_SUPPORTED;
    *definition=proof->source;*declaration=origin->function;return XR_XIR_CTFE_OK;
}
static XrXirStatus instance_inputs_match(const XrXirCompileContext *context,
    const XrXirArtifact *checked,const XrXirArtifact *lowered) {
    if (checked->context.resources!=context->resources ||
        lowered->context.resources!=context->resources) return XR_XIR_BAD_STRUCTURE;
    XrXirCheckedPacket current={0};XrXirStatus status=instance_packet_write(checked,&current);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(lowered,NULL);
    if (status==XR_XIR_OK) {
        const XrXirCheckedPacket *original=&lowered->checked_packet;
        if (current.length!=original->length || !original->bytes ||
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
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_instance(const XirCtfeInstanceRequest *request,XrCompileRequestControl *control,
    XirCtfeInstanceResult **output) {
    if (!request || !request->checked || !output || *output || !instance_limits_valid(request))
        return XR_XIR_CTFE_BAD_ARGUMENT;
    XrXirCtfeStatus controlled=instance_control_poll(control);
    if (controlled!=XR_XIR_CTFE_OK) return controlled;
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

    owner->context=input->context;owner->function=request->function;
    XrXirCheckedPacket packet={0};status=instance_packet_write(input,&packet);
    if (status==XR_XIR_OK) status=instance_packet_read(&owner->context,input,&packet,&owner->checked);
    xr_xir_compile_checked_packet_free(&packet);
    if (status==XR_XIR_OK) {
        controlled=instance_control_poll(control);
        if (controlled!=XR_XIR_CTFE_OK) { xr_xir_compile_ctfe_instance_free(owner);return controlled; }
        status=xr_xir_compile_lower(owner->checked,&request->target, control,&owner->lowered,NULL);
    }
    if (status!=XR_XIR_OK) { xr_xir_compile_ctfe_instance_free(owner);return instance_compile_status(status); }
    XrXirCtfeCallRequest call={.evaluation={.artifact=owner->lowered,.function=request->function,
        .target=request->target,.limits=request->limits,.control=control},.call_depth=request->call_depth};

    memcpy(call.evaluation.checked_identity,owner->lowered->checked_identity,sizeof(call.evaluation.checked_identity));
    admitted=xr_xir_compile_ctfe_calls(&call,&owner->value);
    if (admitted!=XR_XIR_CTFE_OK) { xr_xir_compile_ctfe_instance_free(owner);return admitted; }

    *output=owner;return XR_XIR_CTFE_OK;
}
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_instance_view(const XirCtfeInstanceResult *owner,
    XirCtfeInstanceView *output) {
    if (!owner || !output || output->checked || output->lowered || output->definition || output->value ||
        output->instance_function || output->definition_function) return XR_XIR_CTFE_BAD_ARGUMENT;

    XrXirStatus status=instance_inputs_match(&owner->context,owner->checked,owner->lowered);
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
#include "xxir_ctfe_instance_batch.inc.c"
