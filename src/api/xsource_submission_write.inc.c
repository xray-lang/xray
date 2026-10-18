/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_submission_write.inc.c - Host writes through authentic birth functions
 */
static XrSourceStatus submission_write_binding(XrSourceSubmission *owner,const char *name,
    const XrXirValue *value,XrXirCallStatus *cleanup) {
    XrXirSourceGenerationBinding selected={0};
    XrSourceStatus status=submission_xir(xr_xir_compile_source_generation_lookup(owner->source,name,&selected));
    if(status!=XR_SOURCE_OK)return status;
    XrXirSourceGenerationDefinition definition={0};
    status=submission_xir(xr_xir_compile_source_generation_definition(owner->source,
        selected.generation,selected.declaration->name,&definition));
    if(status!=XR_SOURCE_OK)return status;
    if(!definition.mutable || definition.writer==UINT32_MAX)return XR_SOURCE_REJECTED;
    const SubmissionBirth *birth=owner->published_births;
    while(birth && birth->source!=selected.generation) {
        if(xr_compile_state_work(owner->state,1)!=XR_COMPILE_RESOURCE_OK)
            return submission_resource(xr_compile_state_status(owner->state));
        birth=birth->previous;
    }
    if(!birth)return XR_SOURCE_BAD_STATE;
    uint8_t key[16];status=submission_xir(submission_accessor_key(owner,selected.generation,definition.writer,key));
    if(status!=XR_SOURCE_OK)return status;
    const XrXirInstanceBinding *binding=NULL;
    status=submission_call(xr_xir_instance_binding_lookup(owner->runtime,(const char *)key,sizeof(key),&binding));
    if(status!=XR_SOURCE_OK)return status;
    XrXirInstanceBindingView view={0};
    status=submission_call(xr_xir_instance_binding_view(owner->runtime,binding,&view));
    if(status!=XR_SOURCE_OK)return status;
    uint32_t parameters=definition.logical_type==XR_XIR_UNIT ? 0u : 1u;
    if(view.generation!=birth->runtime || view.kind!=XR_XIR_INSTANCE_BINDING_FUNCTION ||
        view.parameter_count!=parameters || view.result!=XR_XIR_UNIT)return XR_SOURCE_BAD_STATE;
    if(!parameters && (value->type || value->reserved || value->payload))return XR_SOURCE_BAD_ARGUMENT;
    XrXirCallStatus started=xr_xir_instance_binding_start(owner->runtime,binding,parameters ? value : NULL,parameters);
    if(started!=XR_XIR_CALL_READY)return submission_call(started);
    XrXirCallStatus terminal;
    do{terminal=xr_xir_instance_poll_bounded(owner->runtime,256).outcome.status;}while(terminal==XR_XIR_CALL_READY);
    XR_CHECK(terminal!=XR_XIR_CALL_SUSPENDED,"a verified binding writer cannot suspend");
    if(terminal==XR_XIR_CALL_RETURNED) {
        XrXirValue result={0};XrXirCallStatus taken=xr_xir_instance_take_result(owner->runtime,&result);
        status=taken==XR_XIR_CALL_RETURNED ? XR_SOURCE_OK : submission_call(taken);
        xr_xir_value_drop(&result);
    } else status=submission_call(terminal);
    *cleanup=xr_xir_instance_close_activation(owner->runtime);
    return status==XR_SOURCE_OK ? submission_call(*cleanup) : status;
}
XR_FUNC XrSourceStatus xr_source_submission_write(XrSourceSubmission *owner,const char *name,
    const XrXirValue *value,XrXirCallStatus *cleanup) {
    if(!owner || !name || !value || !cleanup)return XR_SOURCE_BAD_ARGUMENT;
    if(owner->busy || owner->source_serial)return XR_SOURCE_BUSY;
    XrSourceStatus status=submission_resource(xr_compile_state_status(owner->state));
    if(status!=XR_SOURCE_OK)return status;
    owner->busy=true;*cleanup=XR_XIR_CALL_READY;
    status=submission_capture(owner,submission_write_binding(owner,name,value,cleanup));
    owner->busy=false;return status;
}
