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
#include "xxir_receive_control.h"
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
static XrXirStatus instance_compile_failure(XrXirCtfeStatus status) {
    return status==XR_XIR_CTFE_OK?XR_XIR_OK:status==XR_XIR_CTFE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
        status==XR_XIR_CTFE_COMPLEXITY_LIMIT?XR_XIR_BUDGET:status==XR_XIR_CTFE_MEMORY_LIMIT?XR_XIR_MEMORY_LIMIT:
        status==XR_XIR_CTFE_OVERFLOW?XR_XIR_OVERFLOW:status==XR_XIR_CTFE_CANCELLED?XR_XIR_CANCELLED:
        status==XR_XIR_CTFE_DEADLINE?XR_XIR_DEADLINE:status==XR_XIR_CTFE_CLOCK_FAILURE?XR_XIR_CLOCK_FAILURE:
        status==XR_XIR_CTFE_PROFILE?XR_XIR_BAD_LAYOUT:XR_XIR_BAD_STRUCTURE;
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
static XrXirStatus instance_packet_write(const XrXirArtifact *artifact,XrXirCheckedPacket *packet,
    XrCompileRequestControl *control) {
    return artifact->evaluation_only?xir_evaluation_write(artifact,packet,NULL,control):
        xir_checked_write(artifact,packet,NULL,control);
}
static XrXirStatus instance_packet_read(const XrXirCompileContext *context,const XrXirArtifact *input,
    const XrXirCheckedPacket *packet,XrXirArtifact **output,XrCompileRequestControl *control) {
    return input->evaluation_only?xir_evaluation_read(context,packet->bytes,packet->length,output,NULL,control):
        xir_checked_read(context,packet->bytes,packet->length,output,NULL,control);
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
static XrXirStatus instance_inputs_preflight(const XrXirCompileContext *context,
    const XrXirArtifact *checked,const XrXirArtifact *lowered,XrCompileRequestControl *control,
    const XirReadRequest *read_request) {
    if (checked->context.resources!=context->resources ||
        lowered->context.resources!=context->resources) return XR_XIR_BAD_STRUCTURE;
    XrXirCheckedPacket current={0};
    XrXirStatus status=xir_checked_write_borrowed(context,checked,checked->evaluation_only!=0,&current,NULL,read_request);
    if (status==XR_XIR_OK) status=xir_artifact_verify(context,lowered,NULL,control,read_request);
    if (status==XR_XIR_OK) {
        const XrXirCheckedPacket *original=&lowered->checked_packet;
        if (current.length!=original->length || !original->bytes) status=XR_XIR_BAD_STRUCTURE;
        else {
            int order=0;status=xir_receive_compare(control,current.bytes,original->bytes,current.length,&order);
            if (status==XR_XIR_OK && order) status=XR_XIR_BAD_STRUCTURE;
        }
    }
    xr_xir_compile_checked_packet_free(&current);return status;
}
/* These are bounds of current owned storage, never a permission receipt.
 * Callers repeat them after callbacks before following any child pointer. */
static XrXirStatus instance_owned_span(const XrXirCompileContext *context,
    const void *memory,uint32_t count,size_t width) {
    if(!xir_compile_context_valid(context) || !memory || !count || !width) return XR_XIR_BAD_STRUCTURE;
    if((uint64_t)count>SIZE_MAX/width) return XR_XIR_OVERFLOW;
    return xir_compile_resource_status(xr_compile_resources_allocation(context->resources,memory,(size_t)count*width));
}
static XrXirStatus instance_input_owners(const XrXirCompileContext *context,
    const XrXirArtifact *checked,const XrXirArtifact *lowered) {
    XrXirStatus status=instance_owned_span(context,checked,1,sizeof(*checked));
    if(status==XR_XIR_OK) status=instance_owned_span(context,lowered,1,sizeof(*lowered));
    if(status!=XR_XIR_OK) return status;
    return checked->context.resources==context->resources && lowered->context.resources==context->resources?
        XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus instance_inputs_prepared_match(const XrXirCompileContext *context,
    const XrXirArtifact *checked,const XrXirArtifact *lowered,const XirReadRequest *request) {
    if(!request || !request->attempt || !request->attempt->active) return XR_XIR_BAD_STRUCTURE;
    XirReadRequest fresh=*request;fresh.control=NULL;
    XrXirStatus status=instance_input_owners(context,checked,lowered);
    if(status==XR_XIR_OK) status=xir_artifact_verify(context,lowered,NULL,NULL,&fresh);
    if(status==XR_XIR_OK) status=xir_artifact_verify(context,checked,NULL,NULL,&fresh);
    if(status==XR_XIR_OK) status=instance_input_owners(context,checked,lowered);
    if(status==XR_XIR_OK) status=xir_checked_match_prepared(context,checked,&lowered->checked_packet,&fresh);
    return status;
}
XR_FUNC void xr_xir_compile_ctfe_instance_free(XirCtfeInstanceResult *owner) {
    if (!owner) return;
    xr_xir_value_drop(&owner->value);
    xr_xir_compile_artifact_free(owner->lowered);
    xr_xir_compile_artifact_free(owner->checked);
    xr_compile_resources_free(owner);
}
static XrXirCtfeStatus instance_create_once(const XirCtfeInstanceRequest *request,XrCompileRequestControl *control,
    XirCtfeInstanceResult **output,const XirReadRequest *read_request) {
    if (!request || !request->checked || !output || *output || !instance_limits_valid(request))
        return XR_XIR_CTFE_BAD_ARGUMENT;
    XrXirCtfeStatus controlled=instance_control_poll(control);
    if (controlled!=XR_XIR_CTFE_OK) return controlled;
    const XrXirArtifact *input=request->checked;
    if (!xir_compile_context_valid(&input->context)) return XR_XIR_CTFE_BAD_ARTIFACT;
    XrXirStatus status=xir_artifact_verify(&input->context,input,NULL,control,read_request);
    if (status!=XR_XIR_OK) return instance_compile_status(status);
    const XrXirArtifact *definition=NULL;uint32_t declaration=0;
    XrXirCtfeStatus admitted=instance_definition(input,request->function,&definition,&declaration);
    if (admitted!=XR_XIR_CTFE_OK) return admitted;
    (void)definition;(void)declaration;
    XirCtfeInstanceResult *owner=xir_type_copy_zero(&input->context,1,sizeof(*owner),&status,control);
    if (!owner) return instance_compile_status(status);

    owner->context=input->context;owner->function=request->function;
    XrXirCheckedPacket packet={0};status=instance_packet_write(input,&packet,control);
    if (status==XR_XIR_OK) status=instance_packet_read(&owner->context,input,&packet,&owner->checked,control);
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

    controlled=instance_control_poll(control);
    if (controlled!=XR_XIR_CTFE_OK) {xr_xir_compile_ctfe_instance_free(owner);return controlled;}
    *output=owner;return XR_XIR_CTFE_OK;
}
static XrXirCtfeStatus instance_single_finish(const XrXirCompileContext *context,
    const XirCtfeInstanceResult *owner,XirCtfeInstanceView *output,const XirReadRequest *request) {
    XrXirStatus status=instance_inputs_prepared_match(context,owner->checked,owner->lowered,request);
    if(status!=XR_XIR_OK) return instance_compile_status(status);
    const XrXirArtifact *definition=NULL;uint32_t declaration=0;
    XrXirCtfeStatus admitted=instance_definition(owner->checked,owner->function,&definition,&declaration);
    if(admitted!=XR_XIR_CTFE_OK) return admitted;
    if(owner->function>=owner->lowered->module.function_count || !xr_xir_value_valid(&owner->value) ||
        owner->lowered->module.functions[owner->function].result!=(XrXirType)owner->value.type)
        return XR_XIR_CTFE_BAD_ARTIFACT;
    if(output->checked || output->lowered || output->definition || output->value ||
        output->instance_function || output->definition_function) return XR_XIR_CTFE_BAD_ARGUMENT;
    *output=(XirCtfeInstanceView){owner->checked,owner->lowered,definition,&owner->value,owner->function,declaration};
    return XR_XIR_CTFE_OK;
}
static bool instance_request_same(const XirCtfeInstanceRequest *a,const XirCtfeInstanceRequest *b) {
    return a->checked==b->checked && a->function==b->function && a->target.architecture==b->target.architecture &&
        a->target.abi_version==b->target.abi_version && a->limits.steps==b->limits.steps &&
        a->limits.frame_bytes==b->limits.frame_bytes && a->limits.output_bytes==b->limits.output_bytes &&
        a->call_depth==b->call_depth;
}
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_instance(const XirCtfeInstanceRequest *request,XrCompileRequestControl *control,
    XirCtfeInstanceResult **output) {
    if(!request || !request->checked || !output || *output || !instance_limits_valid(request) ||
        !xir_compile_context_valid(&request->checked->context)) return XR_XIR_CTFE_BAD_ARGUMENT;
    const XirCtfeInstanceRequest received=*request;
    XirReadOperation operation={.capacity={.resources=received.checked->context.resources}};
    const XirReadRequest read={.operation=&operation,.control=control};
    XirCtfeInstanceResult *owner=NULL;XrXirCtfeStatus result=instance_create_once(&received,control,&owner,&read);
    if(result==XR_XIR_CTFE_OK) {
        const XrXirCompileContext *context=&received.checked->context;
        XirReadAttempt attempt={true};XirReadRequest fresh=read;fresh.control=NULL;fresh.attempt=&attempt;
        for(;;) {
            XrXirStatus status=xir_read_begin(context,&operation);
            if(status==XR_XIR_OK) status=xir_receive_poll(control);
            if(status==XR_XIR_OK && (!instance_request_same(request,&received) || *output)) status=XR_XIR_BAD_STRUCTURE;
            if(status==XR_XIR_OK) status=xir_artifact_verify(context,received.checked,NULL,NULL,&fresh);
            XirCtfeInstanceView view={0};
            if(status==XR_XIR_OK) {result=instance_single_finish(context,owner,&view,&fresh);
                if(result!=XR_XIR_CTFE_OK) status=instance_compile_failure(result);}
            if(status==XR_XIR_OK) status=xir_checked_match_prepared(context,received.checked,&owner->lowered->checked_packet,&fresh);
            XirReadStep step=xir_read_step(&operation,status);
            if(step.kind==XIR_READ_NEED_PREPARE) {
                status=xir_read_prepare(context,&operation,control);
                if(status==XR_XIR_OK) continue;
            }
            result=instance_compile_status(status);break;
        }
    }
    if(result==XR_XIR_CTFE_OK) {*output=owner;owner=NULL;}
    xr_xir_compile_ctfe_instance_free(owner);xir_read_operation_free(&operation);return result;
}
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_instance_view(const XirCtfeInstanceResult *owner,
    XirCtfeInstanceView *output) {
    if(!owner || !output || output->checked || output->lowered || output->definition || output->value ||
        output->instance_function || output->definition_function || !xir_compile_context_valid(&owner->context))
        return XR_XIR_CTFE_BAD_ARGUMENT;
    XirReadOperation operation={.capacity={.resources=owner->context.resources}};
    XirReadAttempt attempt={true};const XirReadRequest request={.operation=&operation,.attempt=&attempt};
    XrXirCtfeStatus result=XR_XIR_CTFE_OK;
    for(;;) {
        XrXirStatus status=xir_read_begin(&owner->context,&operation);
        if(status==XR_XIR_OK) status=instance_input_owners(&owner->context,owner->checked,owner->lowered);
        if(status==XR_XIR_OK) status=instance_inputs_preflight(&owner->context,owner->checked,owner->lowered,NULL,&request);
        if(status==XR_XIR_OK) {
            result=instance_single_finish(&owner->context,owner,output,&request);
            if(result!=XR_XIR_CTFE_OK) status=instance_compile_failure(result);
        }
        XirReadStep step=xir_read_step(&operation,status);
        if(step.kind==XIR_READ_NEED_PREPARE) {
            status=xir_read_prepare(&owner->context,&operation,NULL);
            if(status==XR_XIR_OK) continue;
        }
        if(status!=XR_XIR_OK) {result=instance_compile_status(status);break;}
        result=XR_XIR_CTFE_OK;break;
    }
    xir_read_operation_free(&operation);return result;
}
#include "xxir_ctfe_instance_batch.inc.c"
