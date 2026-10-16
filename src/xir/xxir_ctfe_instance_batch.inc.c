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
static XrXirCtfeStatus instance_batch_receive(const XirCtfeInstanceBatchRequest *request,
    XirCtfeInstanceBatch **output,uint32_t *failed_index) {
    if (!request || !request->checked || !request->functions || !request->count || !output || *output)
        return XR_XIR_CTFE_BAD_ARGUMENT;
    if (failed_index && *failed_index!=UINT32_MAX) return XR_XIR_CTFE_BAD_ARGUMENT;
    XirCtfeInstanceRequest single={request->checked,0,request->target,request->limits,request->call_depth};
    if (!instance_limits_valid(&single)) return XR_XIR_CTFE_BAD_ARGUMENT;
    const XrXirArtifact *input=request->checked;
    if (!xir_compile_context_valid(&input->context)) return XR_XIR_CTFE_BAD_ARTIFACT;
    if (request->count>input->context.limits.functions ||
        (uint64_t)request->count*sizeof(XrXirValue)>SIZE_MAX ||
        (uint64_t)request->count*sizeof(uint32_t)>SIZE_MAX) return XR_XIR_CTFE_BAD_ARGUMENT;
    XrXirStatus status=xr_xir_compile_artifact_verify(input,NULL);
    if (status!=XR_XIR_OK) return instance_compile_status(status);
    if (!xir_compile_work(&input->context,(uint64_t)request->count*sizeof(uint32_t))) return XR_XIR_CTFE_BUDGET;
    for (uint32_t i=0;i<request->count;++i) {
        const XrXirArtifact *definition=NULL;uint32_t declaration=0;
        XrXirCtfeStatus admitted=instance_definition(input,request->functions[i],&definition,&declaration);
        if (admitted!=XR_XIR_CTFE_OK) { if (failed_index) *failed_index=i;return admitted; }
    }
    XirCtfeInstanceBatch *owner=xir_compile_calloc(&input->context,1,sizeof(*owner),&status);
    if (!owner) return instance_compile_status(status);
    if (!xir_compile_work(&input->context,sizeof(owner->context)+sizeof(owner->count))) {
        xr_xir_compile_ctfe_instance_batch_free(owner);return XR_XIR_CTFE_BUDGET;
    }
    owner->context=input->context;owner->count=request->count;
    owner->values=xir_compile_calloc(&input->context,request->count,sizeof(*owner->values),&status);
    if (status==XR_XIR_OK) owner->functions=xir_compile_calloc(&input->context,request->count,sizeof(*owner->functions),&status);
    if (status==XR_XIR_OK && !xir_compile_work(&input->context,(uint64_t)request->count*sizeof(uint32_t))) status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK) memcpy(owner->functions,request->functions,(size_t)request->count*sizeof(uint32_t));
    XrXirCheckedPacket packet={0};
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_write(input,&packet,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(&owner->context,packet.bytes,packet.length,&owner->checked,NULL);
    xr_xir_compile_checked_packet_free(&packet);
    if (status==XR_XIR_OK) status=xr_xir_compile_lower(owner->checked,&request->target,&owner->lowered,NULL);
    if (status!=XR_XIR_OK) { xr_xir_compile_ctfe_instance_batch_free(owner);return instance_compile_status(status); }
    XirCtfeBatchRequest call={owner->lowered,owner->functions,owner->count,request->target,{0},request->limits,request->call_depth};
    if (!xir_compile_work(&owner->context,sizeof(call)+sizeof(call.checked_identity))) {
        xr_xir_compile_ctfe_instance_batch_free(owner);return XR_XIR_CTFE_BUDGET;
    }
    memcpy(call.checked_identity,owner->lowered->checked_identity,sizeof(call.checked_identity));
    XrXirCtfeStatus evaluated=failed_index?xr_xir_compile_ctfe_batch_at(&call,owner->values,failed_index):
        xr_xir_compile_ctfe_batch(&call,owner->values);
    if (evaluated!=XR_XIR_CTFE_OK) { xr_xir_compile_ctfe_instance_batch_free(owner);return evaluated; }
    if (!xir_compile_work(&owner->context,sizeof(*output))) {
        xr_xir_compile_ctfe_instance_batch_free(owner);return XR_XIR_CTFE_BUDGET;
    }
    *output=owner;return XR_XIR_CTFE_OK;
}
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_instance_batch(const XirCtfeInstanceBatchRequest *request,
    XirCtfeInstanceBatch **output) {
    return instance_batch_receive(request,output,NULL);
}
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_instance_batch_at(const XirCtfeInstanceBatchRequest *request,
    XirCtfeInstanceBatch **output,uint32_t *failed_index) {
    return failed_index?instance_batch_receive(request,output,failed_index):XR_XIR_CTFE_BAD_ARGUMENT;
}
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_instance_batch_views(const XirCtfeInstanceBatch *owner,
    XirCtfeInstanceView *output,uint32_t count) {
    if (!owner || !output || count!=owner->count) return XR_XIR_CTFE_BAD_ARGUMENT;
    if (!xir_compile_work(&owner->context,(uint64_t)count*sizeof(*output)*2)) return XR_XIR_CTFE_BUDGET;
    for (uint32_t i=0;i<count;++i)
        if (output[i].checked || output[i].lowered || output[i].definition || output[i].value ||
            output[i].instance_function || output[i].definition_function) return XR_XIR_CTFE_BAD_ARGUMENT;
    XrXirStatus status=instance_inputs_match(&owner->context,owner->checked,owner->lowered);
    if (status!=XR_XIR_OK) return instance_compile_status(status);
    for (uint32_t i=0;i<count;++i) {
        const XrXirArtifact *definition=NULL;uint32_t declaration=0;
        XrXirCtfeStatus admitted=instance_definition(owner->checked,owner->functions[i],&definition,&declaration);
        if (admitted!=XR_XIR_CTFE_OK) return admitted;
        if (owner->lowered->module.functions[owner->functions[i]].result!=(XrXirType)owner->values[i].type)
            return XR_XIR_CTFE_BAD_ARTIFACT;
    }
    /* The admitted immutable graph cannot change between these two passes. */
    for (uint32_t i=0;i<count;++i) {
        const XrXirProvenance *proof=owner->checked->module.provenance;
        output[i]=(XirCtfeInstanceView){owner->checked,owner->lowered,proof->source,&owner->values[i],
            owner->functions[i],proof->origins[owner->functions[i]].function};
    }
    return XR_XIR_CTFE_OK;
}
