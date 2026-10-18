/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "../../module/xmodule_fingerprint.h"
struct XlspSourceCallArguments {
    XlspSourceCallArgumentsView view;
    XlspSourceArgumentSyntax *arguments;
};
static XrXirStatus arguments_resource_status(XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:status==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
XR_FUNC void xlsp_source_call_arguments_free(XlspSourceCallArguments *owner) {
    if(!owner)return;
    xr_compile_resources_free((char *)owner->view.uri);xr_compile_resources_free((void *)owner->view.calls);
    xr_compile_resources_free(owner->arguments);xr_compile_resources_free(owner);
}
XR_FUNC const XlspSourceCallArgumentsView *xlsp_source_call_arguments_view(const XlspSourceCallArguments *owner) {
    return owner?&owner->view:NULL;
}
static XrXirStatus syntax_arguments_copy(const XlspSyntaxSnapshot *s,XlspSourceCallArguments *owner) {
    size_t count=0,arguments=0;
    for(size_t i=0;i<s->call_count;++i) {
        XrXirStatus status=arguments_resource_status(xr_compile_resources_work(owner->view.resources,sizeof(*s->calls)));
        if(status!=XR_XIR_OK)return status;if(!s->calls[i].live)continue;
        if(count==SIZE_MAX||s->calls[i].argument_count>SIZE_MAX-arguments)return XR_XIR_BUDGET;
        ++count;arguments+=s->calls[i].argument_count;
    }
    XlspSourceArgumentCall *calls=NULL;
    XrXirStatus status=count?arguments_resource_status(xr_compile_resources_calloc(owner->view.resources,count,sizeof(*calls),(void **)&calls)):XR_XIR_OK;
    if(status!=XR_XIR_OK)return status;owner->view.calls=calls;owner->view.count=count;
    status=arguments?arguments_resource_status(xr_compile_resources_calloc(owner->view.resources,arguments,sizeof(*owner->arguments),(void **)&owner->arguments)):XR_XIR_OK;
    if(status!=XR_XIR_OK)return status;
    size_t at=0,index=0;
    for(size_t i=0;i<s->call_count;++i) {
        status=arguments_resource_status(xr_compile_resources_work(owner->view.resources,sizeof(*s->calls)));
        if(status!=XR_XIR_OK)return status;
        const XlspSyntaxCallRecord *record=&s->calls[i];if(!record->live)continue;
        XlspSourceArgumentCall *call=&calls[index++];call->call.has_callee=record->has_callee;
        call->call.argument_count=record->argument_count;
        status=syntax_call_range(s,record->boundaries[0],record->closing,&call->call.arguments);
        if(status==XR_XIR_OK&&record->has_callee)status=syntax_call_range(s,record->selection,record->selection_end,&call->call.callee);
        if(status!=XR_XIR_OK)return status;
        call->arguments=record->argument_count?owner->arguments+at:NULL;
        for(size_t p=0;p<record->argument_count;++p) {
            status=arguments_resource_status(xr_compile_resources_work(owner->view.resources,sizeof(*record->arguments)));
            if(status!=XR_XIR_OK)return status;
            const XlspSyntaxArgumentRecord *fact=&record->arguments[p];XlspSourceArgumentSyntax *argument=&owner->arguments[at++];
            argument->has_variable=fact->has_variable;status=syntax_call_range(s,fact->start,fact->end,&argument->range);
            if(status==XR_XIR_OK&&fact->has_variable)status=syntax_call_range(s,fact->variable,fact->variable_end,&argument->variable);
            if(status!=XR_XIR_OK)return status;
        }
    }
    return at==arguments&&index==count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
XR_FUNC XrXirStatus xlsp_source_syntax_arguments(const XlspSyntaxSnapshot *s,XlspSourceCallArguments **out) {
    if(!s||!out||*out)return XR_XIR_BAD_STRUCTURE;
    XrCompileResources *resources=xr_compile_state_resources(s->state);XlspSourceCallArguments *owner=NULL;
    XrXirStatus status=arguments_resource_status(xr_compile_resources_calloc(resources,1,sizeof(*owner),(void **)&owner));
    if(status!=XR_XIR_OK)return status;
    owner->view.resources=resources;owner->view.uri_length=s->uri_length;owner->view.source_length=s->length;owner->view.version=s->version;
    char *uri=NULL;status=arguments_resource_status(xr_compile_resources_alloc(resources,s->uri_length+1,(void **)&uri));
    if(status!=XR_XIR_OK)goto done;owner->view.uri=uri;
    status=arguments_resource_status(xr_compile_resources_work(resources,s->uri_length+1));
    if(status!=XR_XIR_OK)goto done;memcpy(uri,s->uri,s->uri_length+1);
    status=arguments_resource_status(xr_compile_module_source_fingerprint(resources,s->text,&owner->view.fingerprint));
    if(status==XR_XIR_OK)status=syntax_arguments_copy(s,owner);
    if(status==XR_XIR_OK){*out=owner;owner=NULL;}
done:
    xlsp_source_call_arguments_free(owner);return status;
}
