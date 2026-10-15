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
static XrSourceStatus api_xir_status(XrXirStatus status) {
    return status==XR_XIR_OK ? XR_SOURCE_OK : status==XR_XIR_OUT_OF_MEMORY ? XR_SOURCE_OUT_OF_MEMORY :
        status==XR_XIR_BUDGET ? XR_SOURCE_BUDGET : status==XR_XIR_UNSUPPORTED ? XR_SOURCE_UNSUPPORTED : XR_SOURCE_INVALID;
}
static XrSourceStatus api_named_entry(XrSourceProgram *program,const char *name,size_t length,
    uint32_t *output) {
    if(!program || !name || !length || !output)return XR_SOURCE_BAD_ARGUMENT;
    if(length>UINT32_MAX)return XR_SOURCE_LIMIT;
    XrSourceStatus status=api_resource(xr_compile_resources_work(program->resources,length));
    if(status!=XR_SOURCE_OK)return status;
    if(memchr(name,0,length))return XR_SOURCE_BAD_ARGUMENT;
    XrXirSourceProductPacketView packet={0};
    XrXirStatus read=xr_xir_compile_source_product_packet(program->product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet);
    const XrXirCompileContext *context=xr_xir_compile_source_product_context(program->product);
    XrXirArtifact *closed=NULL;
    if(read==XR_XIR_OK)read=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&closed,NULL);
    status=api_xir_status(read);
    uint32_t selected=UINT32_MAX;
    if(status==XR_SOURCE_OK) {
        const XrXirModule *module=xr_xir_compile_artifact_module(closed);
        const XrXirDeclarations *declarations=module->declarations;
        for(uint32_t f=0;f<module->function_count;++f) {
            status=api_resource(xr_compile_resources_work(program->resources,1));
            if(status!=XR_SOURCE_OK)break;
            const XrXirFunctionIdentity *identity=&declarations->functions[f];
            const XrXirFunction *function=&module->functions[f];
            if(identity->module!=declarations->root_module || !identity->exported ||
                identity->method_kind!=XR_XIR_NON_MEMBER || identity->cleanup_owner || function->name_length!=length)continue;
            status=api_resource(xr_compile_resources_work(program->resources,length));
            if(status!=XR_SOURCE_OK)break;
            if(memcmp(function->name,name,length))continue;
            if(selected!=UINT32_MAX){status=XR_SOURCE_INVALID;break;}
            if(function->parameter_count){status=XR_SOURCE_UNSUPPORTED;break;}
            selected=f;
        }
        if(status==XR_SOURCE_OK && selected==UINT32_MAX)status=XR_SOURCE_NOT_FOUND;
    }
    xr_xir_compile_artifact_free(closed);
    if(status==XR_SOURCE_OK)*output=selected;
    return status;
}
XRAY_API XrSourceStatus xray_source_call_begin_named(XrSourceProgram *program,const char *name,
    size_t length,const XrSourceCallOptions *options,XrSourceCall **output) {
    if(!program || !output || *output)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus status=api_options_admit(options);
    uint32_t entry=UINT32_MAX;
    if(status==XR_SOURCE_OK)status=api_named_entry(program,name,length,&entry);
    return status==XR_SOURCE_OK ? api_call_begin_entry(program,entry,options,output) : status;
}
XRAY_API XrSourceStatus xray_source_execute_named(XrSourceProgram *program,const char *name,
    size_t length,const XrSourceCallOptions *options,XrSourceResult **output,XrSourceEvent *failure) {
    if(!program || !output || *output)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus status=api_options_admit(options);
    uint32_t entry=UINT32_MAX;
    if(status==XR_SOURCE_OK)status=api_named_entry(program,name,length,&entry);
    return status==XR_SOURCE_OK ? api_execute_entry(program,entry,options,output,failure) : status;
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
