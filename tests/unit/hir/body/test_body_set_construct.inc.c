/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Array construction publishes complete Set loops and retires failed producers. */
static XrXirStatus collection_source_receive(const XrXirCompileContext *compile, const char *source,
    XrCompileRequestControl *control, XrXirSourceResult *result) {
    XrCompilerSession *session=NULL;
    XrCompileResourceStatus resource=xr_compile_session_new(compile->resources,&session);
    if(resource!=XR_COMPILE_RESOURCE_OK)return resource==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
    if(control)resource=xr_compile_session_bind_request(session,control);
    if(resource!=XR_COMPILE_RESOURCE_OK){xr_compile_session_free(session);return XR_XIR_BAD_STRUCTURE;}
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"collection-construction",NULL};
    XrXirSourceRequest request={session,NULL,&authority,compile,NULL,NULL,XR_XIR_PROGRAM,NULL, XR_XIR_HOST_TARGET};
    XrXirSourceText text={NULL,source,strlen(source)};XrXirSourceFailure failure={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&text,result,NULL,&failure);
    xr_xir_compile_source_failure_free(&failure);xr_compile_session_free(session);return status;
}
static void collection_source_retire(XrXirSourceResult *result) {
    xr_xir_compile_source_snapshot_free(result->snapshot);xr_xir_compile_artifact_free(result->checked);
    *result=(XrXirSourceResult){0};
}
static void set_construct_receivers(void) {
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext compile={.resources=resources,.limits=xr_xir_compile_default_limits()};
    char source[]="fn values()->Array<i64>{const first=[1,2];return [...first,1]}\nfn main(){const value=Set(values());print(len(value))}\nmain()\n";
    XrXirSourceResult result={0};size_t first=physical_attempts;
    CHECK(collection_source_receive(&compile,source,NULL,&result)==XR_XIR_OK);
    size_t sites=physical_attempts-first;CHECK(sites>0);memset(source,0xa5,sizeof(source));
    const XrXirModule *module=xr_xir_compile_artifact_module(result.checked);CHECK(module);
    uint32_t factory=UINT32_MAX,creates=0,inserts=0,calls=0;
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==6 && !memcmp(module->functions[f].name,"values",6))factory=f;
    CHECK(factory!=UINT32_MAX);
    for(uint32_t f=0;f<module->function_count;++f)for(uint32_t n=0;n<module->functions[f].instruction_count;++n) {
        XrXirInstruction op=module->functions[f].instructions[n];
        creates+=op.op==XR_XIR_SET_NEW;inserts+=op.op==XR_XIR_SET_ADD;
        calls+=op.op==XR_XIR_CALL && op.immediate==(int64_t)factory;
    }
    CHECK(creates==1 && inserts==1 && calls==1 && xr_xir_compile_artifact_verify(result.checked,NULL)==XR_XIR_OK);
    collection_source_retire(&result);size_t live=physical_live,bytes=physical_bytes;
    memcpy(source,"fn values()->Array<i64>{const first=[1,2];return [...first,1]}\nfn main(){const value=Set(values());print(len(value))}\nmain()\n",sizeof(source));
    for(size_t point=0;point<sites;++point) {
        physical_fail=physical_attempts+point;XrXirStatus status=collection_source_receive(&compile,source,NULL,&result);physical_fail=SIZE_MAX;
        if(status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"Set source fault %zu/%zu status %u\n",point,sites,(unsigned)status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && !result.checked && !result.snapshot);
        CHECK(physical_live==live && physical_bytes==bytes);
    }
    Cancel cancellation={0,SIZE_MAX};XrCompileRequestOptions options={.cancel_poll=cancel,.cancel_context=&cancellation};
    XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(collection_source_receive(&compile,source,&control,&result)==XR_XIR_OK);
    size_t polls=cancellation.polls;collection_source_retire(&result);CHECK(polls>0);
    for(size_t point=1;point<=polls;++point) {
        cancellation=(Cancel){0,point};control=(XrCompileRequestControl){0};
        CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        CHECK(collection_source_receive(&compile,source,&control,&result)==XR_XIR_CANCELLED && !result.checked && !result.snapshot);
        CHECK(physical_live==live && physical_bytes==bytes);
    }
    xr_compile_resources_release(resources);CHECK(!physical_live && !physical_bytes);
}
