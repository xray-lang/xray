/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_instance_api.inc.c - Public persistent execution and callable authority
 *
 * KEY CONCEPT:
 *   The real host owner keeps one Domain, initialization and cumulative budget.
 */
XRAY_API XrSourceStatus xray_source_instance_new(XrSourceExecution *execution,XrSourceArtifact *program,
    const XrSourceCallOptions *options,XrSourceInstance **output) {
    if(!execution || !program || !output || *output)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus status=api_options_admit(options);
    if(status!=XR_SOURCE_OK)return status;
    if(options->metadata_limit<sizeof(XrSourceInstance)+sizeof(XrSourceCall))return XR_SOURCE_LIMIT;
    if(program->references==UINT32_MAX)return XR_SOURCE_LIMIT;
    XrSourceInstance *owner=NULL;
    status=api_resource(xr_compile_resources_calloc(execution->resources,1,sizeof(*owner),(void **)&owner));
    if(status!=XR_SOURCE_OK)return status;
    owner->resources=execution->resources;
    XrXirInstanceConfig config;
    status=api_config(options,&owner->sink,&config);
    if(status==XR_SOURCE_OK)status=api_ready_vm(program);
    if(status==XR_SOURCE_OK) {
        config.metadata_limit-=sizeof(*owner);
        status=api_call(xr_xir_host_instance_new(program->code,&config,&owner->host));
    }
    if(status!=XR_SOURCE_OK){xr_compile_resources_free(owner);return status;}
    ++program->references;owner->program=program;
    *output=owner;return XR_SOURCE_OK;
}
static XrSourceStatus api_instance_admit(XrSourceInstance *owner) {
    if(!owner)return XR_SOURCE_BAD_ARGUMENT;
    return owner->busy || owner->active || owner->sink.busy ? XR_SOURCE_BUSY : XR_SOURCE_OK;
}
static XrSourceStatus api_instance_begin(XrSourceInstance *owner,const ApiEntry *entry,
    const XrSourceValue *function,XrSourceCall **output) {
    XrSourceCall *call=NULL;
    XrSourceStatus status=api_resource(xr_compile_resources_calloc(owner->resources,1,sizeof(*call),(void **)&call));
    if(status!=XR_SOURCE_OK)return status;
    call->resources=owner->resources;call->owner=owner;
    XrXirCallStatus started=function ? xr_xir_host_instance_begin_function(owner->host,&function->value,
        entry->arguments,entry->argument_count,&call->host) :
        xr_xir_host_instance_begin(owner->host,entry->entry,entry->arguments,entry->argument_count,&call->host);
    status=api_call(started);
    if(status==XR_SOURCE_OK){owner->active=call;*output=call;}else xr_compile_resources_free(call);
    return status;
}
XRAY_API XrSourceStatus xray_source_instance_begin_main(XrSourceInstance *owner,XrSourceCall **output) {
    if(!output || *output)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus status=api_instance_admit(owner);if(status!=XR_SOURCE_OK)return status;
    owner->busy=true;
    const ApiEntry entry={owner->program,owner->resources,owner->program->info.entry,NULL,0};
    status=api_instance_begin(owner,&entry,NULL,output);
    owner->busy=false;return status;
}
XRAY_API XrSourceStatus xray_source_instance_begin(XrSourceInstance *owner,
    const XrSourceInvocation *invocation,XrSourceCall **output) {
    if(!output || *output)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus status=api_instance_admit(owner);if(status!=XR_SOURCE_OK)return status;
    owner->busy=true;ApiEntry entry={0};XrXirValue *arguments=NULL;
    status=api_invocation_entry(&(ApiInvocation){owner->program,owner->resources,invocation},&entry,&arguments);
    if(status==XR_SOURCE_OK)status=api_instance_begin(owner,&entry,NULL,output);
    xr_compile_resources_free(arguments);owner->busy=false;return status;
}
XRAY_API XrSourceStatus xray_source_instance_begin_function(XrSourceInstance *owner,
    const XrSourceValue *function,const XrSourceValue *const *values,size_t count,XrSourceCall **output) {
    if(!function || !function->resources || !output || *output)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus status=api_instance_admit(owner);if(status!=XR_SOURCE_OK)return status;
    owner->busy=true;XrXirValue *arguments=NULL;
    status=api_arguments(owner->resources,values,count,&arguments);
    const ApiEntry entry={owner->program,owner->resources,0,arguments,(uint32_t)count};
    if(status==XR_SOURCE_OK)status=api_instance_begin(owner,&entry,function,output);
    xr_compile_resources_free(arguments);owner->busy=false;return status;
}
XRAY_API XrSourceStatus xray_source_instance_execute(XrSourceInstance *owner,
    const XrSourceInvocation *invocation,XrSourceResult **output,XrSourceEvent *failure) {
    if(!output || *output)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus status=api_instance_admit(owner);if(status!=XR_SOURCE_OK)return status;
    owner->busy=true;ApiEntry entry={0};XrXirValue *arguments=NULL;XrSourceResult *result=NULL;
    status=api_invocation_entry(&(ApiInvocation){owner->program,owner->resources,invocation},&entry,&arguments);
    if(status==XR_SOURCE_OK)status=api_resource(xr_compile_resources_calloc(owner->resources,1,sizeof(*result),(void **)&result));
    if(status==XR_SOURCE_OK) {
        result->resources=owner->resources;
        XrXirCallStatus executed=xr_xir_host_instance_execute(owner->host,entry.entry,entry.arguments,
            entry.argument_count,&result->value);
        if(result->value.status!=XR_XIR_CALL_READY && executed==result->value.status) {
            *output=result;result=NULL;
        } else {
            if(failure){*failure=api_event(executed,0,0);if(xr_xir_call_panic_status(executed) || executed==XR_XIR_CALL_DEFER_ASYNC)failure->state=XR_SOURCE_RUN_CLEANUP_FAILED;}
            status=api_call(executed);
        }
    }
    xray_source_result_free(result);xr_compile_resources_free(arguments);owner->busy=false;return status;
}
XRAY_API XrSourceStatus xray_source_instance_drop(XrSourceInstance **output,XrSourceEvent *cleanup) {
    if(!output)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceInstance *owner=*output;
    if(!owner){if(cleanup)*cleanup=api_event(XR_XIR_CALL_READY,0,0);return XR_SOURCE_OK;}
    XrSourceStatus status=api_instance_admit(owner);if(status!=XR_SOURCE_OK)return status;
    owner->busy=true;
    XrXirCallStatus dropped=xr_xir_host_instance_drop(owner->host);
    if(dropped==XR_XIR_CALL_BUSY){owner->busy=false;return XR_SOURCE_BUSY;}
    if(cleanup){*cleanup=api_event(dropped,0,0);if(dropped!=XR_XIR_CALL_READY)cleanup->state=XR_SOURCE_RUN_CLEANUP_FAILED;}
    xray_source_artifact_free(owner->program);xr_compile_resources_free(owner);*output=NULL;return XR_SOURCE_OK;
}
