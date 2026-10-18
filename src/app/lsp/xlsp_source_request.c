/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "xlsp_source_request.h"
#include "xlsp_source_buffer.h"
#include <limits.h>
#include <string.h>
#include "../../base/xchecks.h"
struct XlspSourceRequest {
    XrXirCompileContext context;
    XlspSourceDocument *documents;
    XlspSourceBuffer **buffers;
    size_t count,entry;
    XrLspPosition position;
    unsigned mode;
    bool include_declaration,started;
};
static XrXirStatus request_resource(XrCompileResourceStatus s) {
    return s==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:s==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        s==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
XR_FUNC void xlsp_source_request_free(XlspSourceRequest *request) {
    if(!request)return;
    for(size_t i=0;i<request->count;++i) {
        if(request->documents)xr_compile_resources_free((char *)request->documents[i].uri);
        if(request->buffers)xlsp_source_buffer_free(request->buffers[i]);
    }
    xr_compile_resources_free(request->documents);xr_compile_resources_free(request->buffers);
    xr_compile_resources_free(request);
}
XR_FUNC XrXirStatus xlsp_source_request_new(const XlspSourceRequestInput *input,XlspSourceRequest **out) {
    if(!input||!input->context||!input->context->resources||!input->documents||!input->count||
        input->entry>=input->count||input->mode>4||!out||*out)return XR_XIR_BAD_STRUCTURE;
    if(input->count>SIZE_MAX/sizeof(XlspSourceDocument)||input->count>SIZE_MAX/sizeof(XlspSourceBuffer *))return XR_XIR_BUDGET;
    XrCompileResources *resources=input->context->resources;XlspSourceRequest *request=NULL;
    XrXirStatus status=request_resource(xr_compile_resources_calloc(resources,1,sizeof(*request),(void **)&request));
    if(status!=XR_XIR_OK)return status;
    request->context=*input->context;request->count=input->count;request->entry=input->entry;
    request->position=input->position;request->mode=input->mode;request->include_declaration=input->include_declaration;
    status=request_resource(xr_compile_resources_calloc(resources,input->count,sizeof(*request->documents),(void **)&request->documents));
    if(status!=XR_XIR_OK)goto done;
    status=request_resource(xr_compile_resources_calloc(resources,input->count,sizeof(*request->buffers),(void **)&request->buffers));
    if(status!=XR_XIR_OK)goto done;
    for(size_t i=0;i<input->count;++i) {
        const XlspSourceDocument *doc=&input->documents[i];char *path=NULL,*uri=NULL;
        if(!doc->uri||doc->uri_length==SIZE_MAX||doc->version<INT32_MIN||doc->version>INT32_MAX){status=XR_XIR_BAD_STRUCTURE;goto done;}
        status=xlsp_source_uri_path(resources,doc->uri,doc->uri_length,&path);xr_compile_resources_free(path);
        if(status!=XR_XIR_OK)goto done;
        status=xlsp_source_buffer_new(resources,doc->text,doc->length,(int)doc->version,&request->buffers[i]);
        if(status!=XR_XIR_OK)goto done;
        status=request_resource(xr_compile_resources_alloc(resources,doc->uri_length+1,(void **)&uri));
        if(status!=XR_XIR_OK)goto done;
        request->documents[i]=(XlspSourceDocument){uri,xlsp_source_buffer_text(request->buffers[i]),doc->uri_length,doc->length,doc->version};
        status=request_resource(xr_compile_resources_work(resources,doc->uri_length+1));if(status!=XR_XIR_OK)goto done;
        memcpy(uri,doc->uri,doc->uri_length);uri[doc->uri_length]=0;
    }
    *out=request;request=NULL;
done:
    xlsp_source_request_free(request);return status;
}
static bool request_cancelled(const _Atomic bool *cancel) {
    return cancel&&atomic_load_explicit(cancel,memory_order_acquire);
}
static bool request_cancel_poll(const void *context) {
    return request_cancelled((const _Atomic bool *)context);
}
XR_FUNC XrXirStatus xlsp_source_request_run(XlspSourceRequest *request,const _Atomic bool *cancel,XlspSourceRequestResult *out) {
    if(!request||request->started||!out||out->json||out->token_update||out->stage||out->cancelled||out->status!=XR_XIR_OK)return XR_XIR_BAD_STRUCTURE;
    request->started=true;
    XlspSourceRequestResult result={0};XlspSourceWorkspace *workspace=NULL;bool bound=false;
    if(request_cancelled(cancel)){result.cancelled=true;goto done;}
    if(cancel){
        result.status=request_resource(xr_compile_resources_begin_cancel(request->context.resources,request_cancel_poll,(const void *)cancel));
        if(result.status!=XR_XIR_OK)goto done;
        bound=true;
    }
    result.status=xlsp_source_workspace_build(&request->context,request->documents,request->count,&workspace,&result.stage,NULL);
    if(request_cancelled(cancel)){result.cancelled=true;goto done;}
    if(result.status==XR_XIR_OK) {
        const XlspSourceDocument *entry=&request->documents[request->entry];result.stage=7;
        result.status=xlsp_source_workspace_navigation(workspace,entry->uri,entry->uri_length,
            request->position,request->mode,request->include_declaration,&result.json);
        if(result.status==XR_XIR_OK)result.stage=0;
    }
    if(request_cancelled(cancel))result.cancelled=true;
done:
    xlsp_source_workspace_free(workspace);
    if(bound){
        XrCompileResourceStatus ended=xr_compile_resources_end_cancel(request->context.resources,request_cancel_poll,(const void *)cancel);
        XR_CHECK(ended==XR_COMPILE_RESOURCE_OK||ended==XR_COMPILE_RESOURCE_CANCELLED,"Source cancellation scope must retain its binding");
        if(ended==XR_COMPILE_RESOURCE_CANCELLED)result.cancelled=true;
    }
    if(request_cancelled(cancel))result.cancelled=true;
    if(result.cancelled){xjson_free(result.json);result.json=NULL;}
    *out=result;return XR_XIR_OK;
}
