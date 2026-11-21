/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Real Source enum Map proofs retire every failed request and producer. */
#include "api/xsource_compile.h"
#include "xir/xxir_compile_memory.h"
#include "base/xcompile_request.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "../xir_sdk_resource_test.h"
typedef struct EnumKeyCancel {size_t polls,after;} EnumKeyCancel;
static bool enum_key_cancel(const void *opaque) {
    EnumKeyCancel *cancel=(EnumKeyCancel *)opaque;return ++cancel->polls>=cancel->after;
}
static XrXirStatus enum_key_source(const XrXirCompileContext *compile,const char *text,
    XrCompileRequestControl *control,XrXirSourceResult *result) {
    XrCompilerSession *session=NULL;XrCompileResourceStatus resource=xr_compile_session_new(compile->resources,&session);
    if(resource!=XR_COMPILE_RESOURCE_OK)return xir_compile_resource_status(resource);
    if(control)resource=xr_compile_session_bind_request(session,control);
    if(resource!=XR_COMPILE_RESOURCE_OK){xr_compile_session_free(session);return xir_compile_resource_status(resource);}
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"enum-key-domain",NULL};
    XrXirSourceRequest request={session,NULL,&authority,compile,NULL,NULL,XR_XIR_PROGRAM,NULL, XR_XIR_HOST_TARGET};
    XrXirSourceText source={NULL,text,strlen(text)};XrXirSourceFailure failure={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&source,result,NULL,&failure);
    xr_xir_compile_source_failure_free(&failure);xr_compile_session_free(session);return status;
}
static void enum_key_source_retire(XrXirSourceResult *result) {
    xr_xir_compile_source_snapshot_free(result->snapshot);xr_xir_compile_artifact_free(result->checked);*result=(XrXirSourceResult){0};
}
int main(void) {
    static const char original[]="enum Choice { A, B }\nconst first={Choice.A:1}\nconst second:Map<Choice,i64>={Choice.A:1,Choice.B:2}\n";
    char *text=xr_malloc(sizeof(original));CHECK(text);memcpy(text,original,sizeof(original));
    XrCompileResources *resources=sdk_ledger(&sdk_unlimited);
    XrCompileResourceStats initial=sdk_stats(resources);
    XrXirCompileContext compile={resources,xr_xir_compile_default_limits()};XrXirSourceResult result={0};
    size_t first=runtime_attempts;CHECK(enum_key_source(&compile,text,NULL,&result)==XR_XIR_OK);
    size_t sites=runtime_attempts-first;CHECK(sites>0);memset(text,0xa5,sizeof(original));xr_free(text);text=NULL;
    CHECK(xr_xir_compile_artifact_verify(result.checked,NULL)==XR_XIR_OK);enum_key_source_retire(&result);
    CHECK(sdk_stats(resources).held_bytes==initial.held_bytes);
    CHECK(sdk_stats(resources).allocation_count>initial.allocation_count);
    size_t live=runtime_live,bytes=runtime_bytes;
    text=xr_malloc(sizeof(original));CHECK(text);memcpy(text,original,sizeof(original));
    for(size_t point=0;point<sites;++point) {
        runtime_fail_at=runtime_attempts+point;XrXirStatus status=enum_key_source(&compile,text,NULL,&result);runtime_fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_OUT_OF_MEMORY&&!result.checked&&!result.snapshot);
        CHECK(runtime_live==live&&runtime_bytes==bytes&&sdk_stats(resources).held_bytes==initial.held_bytes);
    }
    EnumKeyCancel cancellation={0,SIZE_MAX};XrCompileRequestOptions options={.cancel_poll=enum_key_cancel,.cancel_context=&cancellation};
    XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(enum_key_source(&compile,text,&control,&result)==XR_XIR_OK);size_t polls=cancellation.polls;
    enum_key_source_retire(&result);CHECK(polls>0);
    for(size_t point=1;point<=polls;++point) {
        cancellation=(EnumKeyCancel){0,point};control=(XrCompileRequestControl){0};
        CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        CHECK(enum_key_source(&compile,text,&control,&result)==XR_XIR_CANCELLED&&!result.checked&&!result.snapshot);
        CHECK(runtime_live==live&&runtime_bytes==bytes&&sdk_stats(resources).held_bytes==initial.held_bytes);
    }
    memset(text,0xa5,sizeof(original));xr_free(text);
    CHECK(sdk_stats(resources).held_bytes==initial.held_bytes);
    xr_compile_resources_release(resources);CHECK(!runtime_live&&!runtime_bytes);
    puts("Enum Map Source: owned checked result, every allocation failure and request cancellation PASS");return 0;
}
