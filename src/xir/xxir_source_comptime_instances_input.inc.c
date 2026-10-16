/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_comptime_instances_input.inc.c - Ordinary input for typed staging
 */
XR_FUNC XrXirStatus xr_xir_compile_source_comptime_instances(const XirSourceComptimeRequest *request,
    XirSourceComptimeInstances *output,XrXirSourceDiagnostic *diagnostic) {
    if (!request || !request->source || !xir_compile_context_valid(request->source->context) ||
        !output || output->owner || output->count || (request->text && (request->overlays || request->overlay_count)) ||
        request->limits.steps>XR_XIR_CTFE_MAX_STEPS || request->limits.frame_bytes>XR_XIR_CTFE_MAX_FRAME_BYTES ||
        request->limits.output_bytes>XR_XIR_CTFE_MAX_OUTPUT_BYTES || request->call_depth>XR_XIR_CTFE_MAX_CALL_DEPTH) {
        if (diagnostic) *diagnostic=(XrXirSourceDiagnostic){.status=XR_XIR_BAD_STRUCTURE};
        return XR_XIR_BAD_STRUCTURE;
    }
    XirSourceComptimeInstances result={0};SourceComptimeInstancesOperation operation={request,&result};
    SourceInput input={request->text!=NULL,request->text?*request->text:(XrXirSourceText){0},request->overlays,request->overlay_count,false};
    XrXirSourceResult ignored={0};
    XrXirStatus status=source_check_input(request->source,input,&ignored,diagnostic,NULL,NULL,&operation);
    if (status==XR_XIR_OK && !xir_compile_work(request->source->context,sizeof(*output))) status=XR_XIR_BUDGET;
    if (status!=XR_XIR_OK) {
        xr_xir_compile_source_comptime_instances_free(&result);
        if (diagnostic) diagnostic->status=status;
        return status;
    }
    *output=result;return XR_XIR_OK;
}
