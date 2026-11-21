/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Literal diagnostics own their notes and preserve resource failure priority. */
#include "shared/xerror_codes.h"
static XrXirStatus collection_source_failure(const XrXirCompileContext *compile,char *source,XrCompileRequestControl *control,
    XrXirSourceDiagnostic *diagnostic,XrXirSourceFailure *failure) {
    XrCompilerSession *session=NULL;XrCompileResourceStatus resource=xr_compile_session_new(compile->resources,&session);
    if(resource!=XR_COMPILE_RESOURCE_OK)return resource==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
    if(control)resource=xr_compile_session_bind_request(session,control);
    if(resource!=XR_COMPILE_RESOURCE_OK){xr_compile_session_free(session);return XR_XIR_BAD_STRUCTURE;}
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"collection-diagnostic",NULL};
    XrXirSourceRequest request={session,NULL,&authority,compile,NULL,NULL,XR_XIR_PROGRAM,NULL, XR_XIR_HOST_TARGET};
    XrXirSourceText text={NULL,source,strlen(source)};XrXirSourceResult result={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&text,&result,diagnostic,failure);
    CHECK(!result.checked && !result.snapshot);xr_compile_session_free(session);return status;
}
static void collection_control_receivers(void) {
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext compile={.resources=resources,.limits=xr_xir_compile_default_limits()};
    char source[]="var user={\"name\":\"A\",\"age\":30}\n";
    XrXirSourceFailure failure={0};XrXirSourceDiagnostic diagnostic={0};size_t first=physical_attempts;
    CHECK(collection_source_failure(&compile,source,NULL,&diagnostic,&failure)==XR_XIR_BAD_TYPE);
    size_t sites=physical_attempts-first;CHECK(sites>0 && failure.report);
    memset(source,0xa5,sizeof(source));XrFrontendRoot root={0};XrFrontendLabel label={0};
    CHECK(xr_frontend_report_root(failure.report,0,&root) && root.code==XR_ERR_COLLECTION_LITERAL_TYPES && root.labels==4 &&
        root.primary.line==1 && !strcmp(root.message,"E0326: Map literal values have different types: string and i64"));
    CHECK(xr_frontend_report_label(failure.report,0,1,&label) && strstr(label.message,"struct User { name: string; age: i64 }"));
    CHECK(xr_frontend_report_label(failure.report,0,3,&label) &&
        !strcmp(label.message,"to keep both types in one Map, declare Map<string, string | i64>"));
    CHECK(xr_frontend_report_label(failure.report,0,2,&label) &&
        !strcmp(label.message,"for JSON data, declare the type: var user: JSON.Object = {...}"));
    xr_xir_compile_source_failure_free(&failure);size_t live=physical_live,bytes=physical_bytes;
    memcpy(source,"var user={\"name\":\"A\",\"age\":30}\n",sizeof(source));
    for(size_t point=0;point<sites;++point) {
        physical_fail=physical_attempts+point;diagnostic=(XrXirSourceDiagnostic){0};
        XrXirStatus status=collection_source_failure(&compile,source,NULL,&diagnostic,&failure);physical_fail=SIZE_MAX;
        if(status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"collection diagnostic fault %zu/%zu status %u %s\n",point,sites,(unsigned)status,diagnostic.message);
        CHECK(status==XR_XIR_OUT_OF_MEMORY);xr_xir_compile_source_failure_free(&failure);
        CHECK(physical_live==live && physical_bytes==bytes);
    }
    Cancel cancellation={0,SIZE_MAX};XrCompileRequestOptions options={.cancel_poll=cancel,.cancel_context=&cancellation};
    XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(collection_source_failure(&compile,source,&control,&diagnostic,&failure)==XR_XIR_BAD_TYPE);
    size_t polls=cancellation.polls;xr_xir_compile_source_failure_free(&failure);CHECK(polls>0);
    for(size_t point=1;point<=polls;++point) {
        cancellation=(Cancel){0,point};control=(XrCompileRequestControl){0};
        CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        diagnostic=(XrXirSourceDiagnostic){0};
        XrXirStatus status=collection_source_failure(&compile,source,&control,&diagnostic,&failure);
        CHECK(status==XR_XIR_CANCELLED);xr_xir_compile_source_failure_free(&failure);
        CHECK(physical_live==live && physical_bytes==bytes);
    }
    xr_compile_resources_release(resources);CHECK(!physical_live && !physical_bytes);
}
