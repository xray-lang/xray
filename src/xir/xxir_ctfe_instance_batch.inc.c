/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_instance_batch.inc.c - Shared immutable inputs, independent results
 */
struct XirCtfeInstanceBatch {
    XrXirCompileContext context;
    XrXirArtifact *checked,*lowered;
    XrXirValue *values;
    uint32_t *functions;
    uint32_t count;
};
XR_FUNC void xr_xir_compile_ctfe_instance_batch_free(XirCtfeInstanceBatch *owner) {
    if (!owner) return;
    for (uint32_t i=0;owner->values && i<owner->count;++i) xr_xir_value_drop(&owner->values[i]);
    xr_xir_compile_artifact_free(owner->lowered);xr_xir_compile_artifact_free(owner->checked);
    xr_compile_resources_free(owner->values);xr_compile_resources_free(owner->functions);
    xr_compile_resources_free(owner);
}
static XrXirCtfeStatus instance_batch_receive_once(const XirCtfeInstanceBatchRequest *request,XrCompileRequestControl *control,
    XirCtfeInstanceBatch **output,uint32_t *failed_index,const XirReadRequest *read_request) {
    if (!request || !request->checked || !request->functions || !request->count || !output || *output)
        return XR_XIR_CTFE_BAD_ARGUMENT;
    if (failed_index && *failed_index!=UINT32_MAX) return XR_XIR_CTFE_BAD_ARGUMENT;
    XirCtfeInstanceRequest single={request->checked,0,request->target,request->limits,request->call_depth};
    if (!instance_limits_valid(&single)) return XR_XIR_CTFE_BAD_ARGUMENT;
    XrXirCtfeStatus controlled=instance_control_poll(control);
    if (controlled!=XR_XIR_CTFE_OK) return controlled;
    const XrXirArtifact *input=request->checked;
    if (!xir_compile_context_valid(&input->context)) return XR_XIR_CTFE_BAD_ARTIFACT;
    if (request->count>input->context.limits.functions ||
        (uint64_t)request->count*sizeof(XrXirValue)>SIZE_MAX ||
        (uint64_t)request->count*sizeof(uint32_t)>SIZE_MAX) return XR_XIR_CTFE_BAD_ARGUMENT;
    XrXirStatus status=xir_artifact_verify(&input->context,input,NULL,control,read_request);
    if (status!=XR_XIR_OK) return instance_compile_status(status);

    uint32_t progress=0;
    for (uint32_t i=0;i<request->count;++i) {
        status=xir_receive_step(control,&progress);if (status!=XR_XIR_OK) return instance_compile_status(status);
        const XrXirArtifact *definition=NULL;uint32_t declaration=0;
        XrXirCtfeStatus admitted=instance_definition(input,request->functions[i],&definition,&declaration);
        if (admitted!=XR_XIR_CTFE_OK) { if (failed_index) *failed_index=i;return admitted; }
    }
    XirCtfeInstanceBatch *owner=xir_type_copy_zero(&input->context,1,sizeof(*owner),&status,control);
    if (!owner) return instance_compile_status(status);

    owner->context=input->context;owner->count=request->count;
    owner->values=xir_type_copy_zero(&input->context,request->count,sizeof(*owner->values),&status,control);
    if (status==XR_XIR_OK) owner->functions=xir_type_copy_bytes(&input->context,request->functions,request->count,sizeof(*owner->functions),&status,control);

    XrXirCheckedPacket packet={0};
    if (status==XR_XIR_OK) status=instance_packet_write(input,&packet,control);
    if (status==XR_XIR_OK) status=instance_packet_read(&owner->context,input,&packet,&owner->checked,control);
    xr_xir_compile_checked_packet_free(&packet);
    if (status==XR_XIR_OK) {
        controlled=instance_control_poll(control);
        if (controlled!=XR_XIR_CTFE_OK) { xr_xir_compile_ctfe_instance_batch_free(owner);return controlled; }
        status=xr_xir_compile_lower(owner->checked,&request->target, control,&owner->lowered,NULL);
    }
    if (status!=XR_XIR_OK) { xr_xir_compile_ctfe_instance_batch_free(owner);return instance_compile_status(status); }
    XirCtfeBatchRequest call={owner->lowered,owner->functions,owner->count,request->target,{0},request->limits,request->call_depth};

    memcpy(call.checked_identity,owner->lowered->checked_identity,sizeof(call.checked_identity));
    XrXirCtfeStatus evaluated=failed_index?xr_xir_compile_ctfe_batch_at(&call,control,owner->values,failed_index):
        xr_xir_compile_ctfe_batch(&call,control,owner->values);
    if (evaluated!=XR_XIR_CTFE_OK) { xr_xir_compile_ctfe_instance_batch_free(owner);return evaluated; }

    controlled=instance_control_poll(control);
    if (controlled!=XR_XIR_CTFE_OK) {xr_xir_compile_ctfe_instance_batch_free(owner);return controlled;}
    *output=owner;return XR_XIR_CTFE_OK;
}
static XrXirCtfeStatus instance_batch_receive(const XirCtfeInstanceBatchRequest *request,
    XrCompileRequestControl *control,XirCtfeInstanceBatch **output,uint32_t *failed_index);
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_instance_batch(const XirCtfeInstanceBatchRequest *request,XrCompileRequestControl *control,
    XirCtfeInstanceBatch **output) {
    return instance_batch_receive(request,control,output,NULL);
}
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_instance_batch_at(const XirCtfeInstanceBatchRequest *request,XrCompileRequestControl *control,
    XirCtfeInstanceBatch **output,uint32_t *failed_index) {
    return failed_index?instance_batch_receive(request,control,output,failed_index):XR_XIR_CTFE_BAD_ARGUMENT;
}
static XrXirStatus instance_batch_frame(const XrXirCompileContext *context,
    const XirCtfeInstanceBatch *owner,uint32_t count) {
    if(!owner || !xir_compile_context_valid(context) || owner->context.resources!=context->resources ||
        !count || count!=owner->count) return XR_XIR_BAD_STRUCTURE;
    if(count>context->limits.functions) return XR_XIR_BUDGET;
    XrXirStatus status=instance_owned_span(context,owner->functions,count,sizeof(*owner->functions));
    if(status==XR_XIR_OK) status=instance_owned_span(context,owner->values,count,sizeof(*owner->values));
    if(status==XR_XIR_OK) status=instance_input_owners(context,owner->checked,owner->lowered);
    return status;
}
static bool instance_view_empty(const XirCtfeInstanceView *view) {
    return !view->checked && !view->lowered && !view->definition && !view->value &&
        !view->instance_function && !view->definition_function;
}
static XrXirCtfeStatus instance_batch_row(const XirCtfeInstanceBatch *owner,uint32_t i) {
    uint32_t function=owner->functions[i];
    const XrXirArtifact *definition=NULL;uint32_t declaration=0;
    XrXirCtfeStatus status=instance_definition(owner->checked,function,&definition,&declaration);
    if(status!=XR_XIR_CTFE_OK) return status;
    if(function>=owner->lowered->module.function_count ||
        owner->lowered->module.functions[function].result!=(XrXirType)owner->values[i].type)
        return XR_XIR_CTFE_BAD_ARTIFACT;
    return XR_XIR_CTFE_OK;
}
static XrXirStatus instance_batch_views_finish(const XrXirCompileContext *context,
    const XirCtfeInstanceBatch *owner,XirCtfeInstanceView *output,uint32_t count,
    const XirReadRequest *request,XrXirCtfeStatus *detail) {
    *detail=XR_XIR_CTFE_OK;
    if(!output) {*detail=XR_XIR_CTFE_BAD_ARGUMENT;return XR_XIR_BAD_STRUCTURE;}
    XrXirStatus status=instance_batch_frame(context,owner,count);
    if(status==XR_XIR_OK) status=instance_inputs_prepared_match(context,owner->checked,owner->lowered,request);
    if(status!=XR_XIR_OK) return status;
    for(uint32_t i=0;i<count;++i) {
        if(!instance_view_empty(&output[i])) {*detail=XR_XIR_CTFE_BAD_ARGUMENT;return XR_XIR_BAD_STRUCTURE;}
        *detail=instance_batch_row(owner,i);if(*detail!=XR_XIR_CTFE_OK) return XR_XIR_BAD_TYPE;
    }
    for(uint32_t i=0;i<count;++i) {
        uint32_t function=owner->functions[i];const XrXirProvenance *proof=owner->checked->module.provenance;
        output[i]=(XirCtfeInstanceView){owner->checked,owner->lowered,proof->source,&owner->values[i],
            function,proof->origins[function].function};
    }
    return XR_XIR_OK;
}
static XrXirStatus instance_batch_views_attempt(const XrXirCompileContext *context,
    const XirCtfeInstanceBatch *owner,XirCtfeInstanceView *output,uint32_t count,
    const XirReadRequest *request,XrXirCtfeStatus *detail) {
    *detail=XR_XIR_CTFE_OK;
    if(!output) {*detail=XR_XIR_CTFE_BAD_ARGUMENT;return XR_XIR_BAD_STRUCTURE;}
    XrXirStatus status=instance_batch_frame(context,owner,count);uint32_t progress=0;
    if(status!=XR_XIR_OK) return status;
    for(uint32_t i=0;i<count;++i) {
        if(!instance_view_empty(&output[i])) {*detail=XR_XIR_CTFE_BAD_ARGUMENT;return XR_XIR_BAD_STRUCTURE;}
        status=xir_receive_step(request->control,&progress);if(status!=XR_XIR_OK) return status;
    }
    status=instance_batch_frame(context,owner,count);
    if(status==XR_XIR_OK) status=instance_inputs_preflight(context,owner->checked,owner->lowered,request->control,request);
    if(status!=XR_XIR_OK) return status;
    for(uint32_t i=0;i<count;++i) {
        status=xir_receive_step(request->control,&progress);if(status!=XR_XIR_OK) return status;
        status=instance_batch_frame(context,owner,count);if(status!=XR_XIR_OK) return status;
        *detail=instance_batch_row(owner,i);if(*detail!=XR_XIR_CTFE_OK) return XR_XIR_BAD_TYPE;
    }
    status=xir_receive_poll(request->control);if(status!=XR_XIR_OK) return status;
    return instance_batch_views_finish(context,owner,output,count,request,detail);
}
XR_FUNC XrXirCtfeStatus xir_ctfe_instance_batch_views_prepared(const XrXirCompileContext *context,
    const XirCtfeInstanceBatch *owner,XirCtfeInstanceView *output,uint32_t count,const XirReadRequest *request) {
    if(!request || !request->attempt || !request->attempt->active || !xir_read_request_matches(context,request))
        return XR_XIR_CTFE_BAD_ARGUMENT;
    XrXirCtfeStatus detail=XR_XIR_CTFE_OK;
    XrXirStatus status=instance_batch_views_finish(context,owner,output,count,request,&detail);
    return detail!=XR_XIR_CTFE_OK?detail:instance_compile_status(status);
}
XR_FUNC XrXirCtfeStatus xir_ctfe_instance_batch_views_read(const XrXirCompileContext *context,
    const XirCtfeInstanceBatch *owner,XirCtfeInstanceView *output,uint32_t count,const XirReadRequest *read_request) {
    if(!read_request || !read_request->operation || !xir_read_request_matches(context,read_request))
        return XR_XIR_CTFE_BAD_ARGUMENT;
    bool outer=!read_request->attempt;XirReadAttempt attempt={true};XirReadRequest request=*read_request;
    if(outer) request.attempt=&attempt;
    for(;;) {
        XrXirStatus status=outer?xir_read_begin(context,request.operation):XR_XIR_OK;
        XrXirCtfeStatus detail=XR_XIR_CTFE_OK;
        if(status==XR_XIR_OK) status=instance_batch_views_attempt(context,owner,output,count,&request,&detail);
        XirReadStep step=xir_read_step(request.operation,status);
        if(step.kind!=XIR_READ_NEED_PREPARE || !outer)
            return detail!=XR_XIR_CTFE_OK?detail:instance_compile_status(status);
        status=xir_read_prepare(context,request.operation,request.control);
        if(status!=XR_XIR_OK) return instance_compile_status(status);
    }
}
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_instance_batch_views(const XirCtfeInstanceBatch *owner,
    XirCtfeInstanceView *output,uint32_t count,XrCompileRequestControl *control) {
    if(!owner || !output || count!=owner->count || !xir_compile_context_valid(&owner->context))
        return XR_XIR_CTFE_BAD_ARGUMENT;
    XirReadOperation operation={.capacity={.resources=owner->context.resources}};
    const XirReadRequest request={.operation=&operation,.control=control};
    XrXirCtfeStatus status=xir_ctfe_instance_batch_views_read(&owner->context,owner,output,count,&request);
    xir_read_operation_free(&operation);return status;
}

static bool instance_batch_request_same(const XirCtfeInstanceBatchRequest *a,const XirCtfeInstanceBatchRequest *b) {
    return a->checked==b->checked && a->functions==b->functions && a->count==b->count &&
        a->target.architecture==b->target.architecture && a->target.abi_version==b->target.abi_version &&
        a->limits.steps==b->limits.steps && a->limits.frame_bytes==b->limits.frame_bytes &&
        a->limits.output_bytes==b->limits.output_bytes && a->call_depth==b->call_depth;
}
static XrXirCtfeStatus instance_batch_receive(const XirCtfeInstanceBatchRequest *request,
    XrCompileRequestControl *control,XirCtfeInstanceBatch **output,uint32_t *failed_index) {
    if(!request || !request->checked || !request->functions || !request->count || !output || *output ||
        !xir_compile_context_valid(&request->checked->context) || (failed_index && *failed_index!=UINT32_MAX))
        return XR_XIR_CTFE_BAD_ARGUMENT;
    const XirCtfeInstanceBatchRequest received=*request;
    XirReadOperation operation={.capacity={.resources=received.checked->context.resources}};
    const XirReadRequest read={.operation=&operation,.control=control};
    XirCtfeInstanceBatch *owner=NULL;
    XrXirCtfeStatus result=instance_batch_receive_once(&received,control,&owner,failed_index,&read);
    if(result==XR_XIR_CTFE_OK) {
        const XrXirCompileContext *context=&received.checked->context;
        XirReadAttempt attempt={true};XirReadRequest fresh=read;fresh.control=NULL;fresh.attempt=&attempt;
        for(;;) {
            XrXirStatus status=xir_read_begin(context,&operation);
            if(status==XR_XIR_OK) status=xir_receive_poll(control);
            if(status==XR_XIR_OK && (!instance_batch_request_same(request,&received) || *output ||
                (failed_index && *failed_index!=UINT32_MAX))) status=XR_XIR_BAD_STRUCTURE;
            if(status==XR_XIR_OK) status=instance_batch_frame(context,owner,received.count);
            if(status==XR_XIR_OK) status=xir_artifact_verify(context,received.checked,NULL,NULL,&fresh);
            if(status==XR_XIR_OK) status=instance_inputs_prepared_match(context,owner->checked,owner->lowered,&fresh);
            if(status==XR_XIR_OK) status=xir_checked_match_prepared(context,received.checked,&owner->lowered->checked_packet,&fresh);
            for(uint32_t i=0;status==XR_XIR_OK && i<received.count;++i) {
                if(received.functions[i]!=owner->functions[i] || !xr_xir_value_valid(&owner->values[i])) status=XR_XIR_BAD_STRUCTURE;
                if(status==XR_XIR_OK && instance_batch_row(owner,i)!=XR_XIR_CTFE_OK) status=XR_XIR_BAD_STRUCTURE;
            }
            XirReadStep step=xir_read_step(&operation,status);
            if(step.kind==XIR_READ_NEED_PREPARE) {
                status=xir_read_prepare(context,&operation,control);
                if(status==XR_XIR_OK) continue;
            }
            result=instance_compile_status(status);break;
        }
    }
    if(result==XR_XIR_CTFE_OK) {*output=owner;owner=NULL;}
    xr_xir_compile_ctfe_instance_batch_free(owner);xir_read_operation_free(&operation);return result;
}
