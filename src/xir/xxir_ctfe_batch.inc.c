/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_batch.inc.c - Atomic results from one current immutable artifact
 */
static XrXirCtfeStatus ctfe_batch_received(const XirCtfeBatchRequest *request,XrCompileRequestControl *control,
    XrXirValue *output,uint32_t *failed_index) {
    if (!request || !request->artifact || !request->functions || !request->count || !output ||
        request->limits.steps>XR_XIR_CTFE_MAX_STEPS || request->limits.frame_bytes>XR_XIR_CTFE_MAX_FRAME_BYTES ||
        request->limits.output_bytes>XR_XIR_CTFE_MAX_OUTPUT_BYTES || request->call_depth>XR_XIR_CTFE_MAX_CALL_DEPTH)
        return XR_XIR_CTFE_BAD_ARGUMENT;
    if (failed_index && *failed_index!=UINT32_MAX) return XR_XIR_CTFE_BAD_ARGUMENT;
    const XrXirArtifact *artifact=request->artifact;
    if (!xir_compile_context_valid(&artifact->context)) return XR_XIR_CTFE_BAD_ARTIFACT;
    if (request->count>artifact->context.limits.functions) return XR_XIR_CTFE_BAD_ARGUMENT;
    uint64_t bytes=(uint64_t)request->count*sizeof(*output);
    if (bytes>SIZE_MAX) return XR_XIR_CTFE_OVERFLOW;
    for (uint32_t i=0;i<request->count;++i)
        if (output[i].type || output[i].reserved || output[i].payload) return XR_XIR_CTFE_BAD_ARGUMENT;
    XrXirCtfeStatus status=ctfe_control_poll(control);
    if (status!=XR_XIR_CTFE_OK) return status;
    status=ctfe_compile_status(xr_xir_compile_artifact_verify(artifact,NULL));
    if (status!=XR_XIR_CTFE_OK) return status;
    for (uint32_t i=0;i<request->count;++i) {

        XrXirCtfeCallRequest call={.evaluation={.artifact=artifact,.function=request->functions[i],
            .target=request->target,.limits=request->limits,.control=control},.call_depth=request->call_depth};
        memcpy(call.evaluation.checked_identity,request->checked_identity,32);
        bool owned=false;XrXirCellProvenance *cells=NULL;void *closure=NULL;
        status=ctfe_call_prepare_received(&call,&owned,&cells,&closure);
        if (status==XR_XIR_CTFE_OK && artifact->module.functions[call.evaluation.function].parameter_count)
            status=XR_XIR_CTFE_BAD_ARGUMENT;
        if (status!=XR_XIR_CTFE_OK) {
            if (failed_index) *failed_index=i;
            xr_xir_compile_cell_provenance_free(cells);xr_compile_resources_free(closure);break;
        }
        status=ctfe_call_run_received(&call,&output[i],owned,cells,closure);
        if (status!=XR_XIR_CTFE_OK) { if (failed_index) *failed_index=i;break; }
    }
    if (status!=XR_XIR_CTFE_OK)
        for (uint32_t i=0;i<request->count;++i) xr_xir_value_drop(&output[i]);
    return status;
}
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_batch(const XirCtfeBatchRequest *request,XrCompileRequestControl *control,XrXirValue *output) {
    return ctfe_batch_received(request,control,output,NULL);
}
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_batch_at(const XirCtfeBatchRequest *request,XrCompileRequestControl *control,
    XrXirValue *output,uint32_t *failed_index) {
    return failed_index?ctfe_batch_received(request,control,output,failed_index):XR_XIR_CTFE_BAD_ARGUMENT;
}
