/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_entry_api.inc.c - Exact closed declaration selection and outcome copies
 *
 * KEY CONCEPT:
 *   A name selects a sealed declaration; the actual Instance grants execution.
 */
static XrSourceStatus api_named_entry(XrSourceArtifact *program,const char *name,size_t length,
    uint32_t argument_count,uint32_t *output) {
    if(!program || !name || !length || !output)return XR_SOURCE_BAD_ARGUMENT;
    if(length>UINT32_MAX)return XR_SOURCE_LIMIT;
    XrSourceStatus status=api_ready_vm(program);
    if(status!=XR_SOURCE_OK)return status;
    if(memchr(name,0,length))return XR_SOURCE_BAD_ARGUMENT;
    uint32_t selected=UINT32_MAX;
    for(uint32_t f=0;f<program->name_count;++f) {
        const ApiNamedEntry *entry=&program->names[f];
        if(entry->length!=length || memcmp(entry->name,name,length))continue;
        if(selected!=UINT32_MAX)return XR_SOURCE_INVALID;
        if(entry->arguments!=argument_count)return XR_SOURCE_BAD_ARGUMENT;
        selected=entry->entry;
    }
    if(selected==UINT32_MAX)return XR_SOURCE_NOT_FOUND;
    *output=selected;return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_invocation_init(XrSourceInvocation *invocation,size_t size) {
    if(!invocation || size!=sizeof(*invocation))return XR_SOURCE_BAD_ARGUMENT;
    *invocation=(XrSourceInvocation){.api_version=XRAY_SOURCE_INVOCATION_VERSION,.struct_size=sizeof(*invocation)};
    return XR_SOURCE_OK;
}
static XrSourceStatus api_arguments(XrCompileResources *resources,const XrSourceValue *const *values,
    size_t count,XrXirValue **arguments) {
    if(!resources)return XR_SOURCE_BAD_ARGUMENT;
    if(count>UINT32_MAX || count>SIZE_MAX/sizeof(**arguments))return XR_SOURCE_LIMIT;
    if(count && !values)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus status=XR_SOURCE_OK;
    if(count) {
        status=api_resource(xr_compile_resources_calloc(resources,count,sizeof(**arguments),(void **)arguments));
        if(status!=XR_SOURCE_OK)return status;
        for(uint32_t i=0;i<count;++i) {
            const XrSourceValue *value=values[i];
            if(!value || !value->resources)return XR_SOURCE_BAD_ARGUMENT;
            if(!xr_xir_value_valid(&value->value))return XR_SOURCE_INVALID;
            (*arguments)[i]=value->value;
        }
    }
    return status;
}
typedef struct ApiInvocation {
    XrSourceArtifact *program;
    XrCompileResources *resources;
    const XrSourceInvocation *input;
} ApiInvocation;
static XrSourceStatus api_invocation_entry(const ApiInvocation *request,
    ApiEntry *entry,XrXirValue **arguments) {
    XrSourceArtifact *program=request->program;
    const XrSourceInvocation *invocation=request->input;
    if(!invocation || invocation->api_version!=XRAY_SOURCE_INVOCATION_VERSION ||
        invocation->struct_size!=sizeof(*invocation))return XR_SOURCE_BAD_ARGUMENT;
    if(invocation->argument_count>UINT32_MAX || invocation->argument_count>SIZE_MAX/sizeof(**arguments))return XR_SOURCE_LIMIT;
    if(invocation->argument_count && !invocation->arguments)return XR_SOURCE_BAD_ARGUMENT;
    uint32_t ordinal=UINT32_MAX,count=(uint32_t)invocation->argument_count;
    XrSourceStatus status=api_named_entry(program,invocation->name,invocation->name_length,count,&ordinal);
    if(status==XR_SOURCE_OK)status=api_arguments(request->resources,invocation->arguments,count,arguments);
    if(status!=XR_SOURCE_OK)return status;
    *entry=(ApiEntry){program,request->resources,ordinal,*arguments,count};return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_invoke_begin(XrSourceExecution *execution,XrSourceArtifact *program,const XrSourceInvocation *invocation,
    const XrSourceCallOptions *options,XrSourceCall **output) {
    if(!execution || !program || !output || *output)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus status=api_options_admit(options);ApiEntry entry={0};XrXirValue *arguments=NULL;
    if(status==XR_SOURCE_OK)status=api_invocation_entry(&(ApiInvocation){program,execution->resources,invocation},&entry,&arguments);
    if(status==XR_SOURCE_OK)status=api_call_begin_entry(&entry,options,output);
    xr_compile_resources_free(arguments);return status;
}
XRAY_API XrSourceStatus xray_source_invoke(XrSourceExecution *execution,XrSourceArtifact *program,const XrSourceInvocation *invocation,
    const XrSourceCallOptions *options,XrSourceResult **output,XrSourceEvent *failure) {
    if(!execution || !program || !output || *output)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus status=api_options_admit(options);ApiEntry entry={0};XrXirValue *arguments=NULL;
    if(status==XR_SOURCE_OK)status=api_invocation_entry(&(ApiInvocation){program,execution->resources,invocation},&entry,&arguments);
    if(status==XR_SOURCE_OK)status=api_execute_entry(&entry,options,output,failure);
    xr_compile_resources_free(arguments);return status;
}
XRAY_API XrSourceStatus xray_source_result_copy(const XrSourceResult *source,XrSourceResult **output) {
    if(!source || !source->resources || !output || *output)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceResult *result=NULL;
    XrSourceStatus status=api_resource(xr_compile_resources_calloc(source->resources,1,sizeof(*result),(void **)&result));
    if(status!=XR_SOURCE_OK)return status;
    result->resources=source->resources;
    XrXirValueStatus copied=xr_xir_call_result_copy(&source->value,&result->value);
    if(copied!=XR_XIR_VALUE_OK) {
        xray_source_result_free(result);
        return copied==XR_XIR_VALUE_OOM ? XR_SOURCE_OUT_OF_MEMORY :
            copied==XR_XIR_VALUE_LIMIT || copied==XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_SOURCE_LIMIT : XR_SOURCE_INVALID;
    }
    *output=result;return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_result_bool(const XrSourceResult *result,int *output) {
    if(!result || !output)return XR_SOURCE_BAD_ARGUMENT;
    if(result->value.status!=XR_XIR_CALL_RETURNED || result->value.value.type!=XR_XIR_BOOL)return XR_SOURCE_INVALID;
    *output=result->value.value.payload ? 1 : 0;return XR_SOURCE_OK;
}
XRAY_API XrSourceStatus xray_source_result_f64(const XrSourceResult *result,double *output) {
    if(!result || !output)return XR_SOURCE_BAD_ARGUMENT;
    if(result->value.status!=XR_XIR_CALL_RETURNED || result->value.value.type!=XR_XIR_F64)return XR_SOURCE_INVALID;
    _Static_assert(sizeof(double)==sizeof(result->value.value.payload),"the public double carrier is binary64");
    memcpy(output,&result->value.value.payload,sizeof(*output));return XR_SOURCE_OK;
}
