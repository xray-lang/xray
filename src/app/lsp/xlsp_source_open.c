/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "xlsp_source_open.h"
#include <limits.h>
#include <string.h>
struct XlspSourceOpen {
    XrCompileResources *resources;
    char *uri;
    size_t uri_length;
    XlspSourceBuffer *buffer;
    XlspSyntaxSnapshot *syntax;
    XlspSourceWorkspace *workspace;
    XlspSourceWorkspaceFailure failure;
    XrXirStatus query_status;
    char *diagnostic_uri;
    size_t diagnostic_uri_length;
    XlspSourceBuffer *diagnostic_buffer;
    int diagnostic_version;
    bool diagnostic_resolved;
};
static XrXirStatus open_status(XrCompileResourceStatus s) {
    return s==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:s==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        s==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
void xlsp_source_open_free(XlspSourceOpen *o) {
    if(!o)return;
    xlsp_source_buffer_free(o->buffer);xlsp_source_syntax_free(o->syntax);
    xlsp_source_workspace_free(o->workspace);xlsp_source_workspace_failure_free(&o->failure);
    xlsp_source_buffer_free(o->diagnostic_buffer);xr_compile_resources_free(o->diagnostic_uri);
    xr_compile_resources_free(o->uri);xr_compile_resources_free(o);
}
const char *xlsp_source_open_uri(const XlspSourceOpen *o){return o?o->uri:NULL;}
XrXirStatus xlsp_source_open_query_status(const XlspSourceOpen *o){return o?o->query_status:XR_XIR_BAD_STRUCTURE;}
const XlspSourceWorkspaceFailure *xlsp_source_open_failure(const XlspSourceOpen *o){return o?&o->failure:NULL;}
XlspSourceBuffer *xlsp_source_open_take_buffer(XlspSourceOpen *o){if(!o)return NULL;XlspSourceBuffer *p=o->buffer;o->buffer=NULL;return p;}
XlspSyntaxSnapshot *xlsp_source_open_take_syntax(XlspSourceOpen *o){if(!o)return NULL;XlspSyntaxSnapshot *p=o->syntax;o->syntax=NULL;return p;}
XlspSourceWorkspace *xlsp_source_open_take_workspace(XlspSourceOpen *o){if(!o)return NULL;XlspSourceWorkspace *p=o->workspace;o->workspace=NULL;return p;}
static XrXirStatus open_path_equal(XrCompileResources *resources,const char *a,const char *b,bool *equal) {
    *equal=false;
    for(size_t i=0;;++i) {
        XrXirStatus status=open_status(xr_compile_resources_work(resources,2));
        if(status!=XR_XIR_OK)return status;
        char first=a[i],second=b[i];
#if defined(XR_OS_WINDOWS)
        if(first=='\\')first='/';if(second=='\\')second='/';
#endif
        if(first!=second)return XR_XIR_OK;
        if(!first){*equal=true;return XR_XIR_OK;}
    }
}
static XrXirStatus open_capture_diagnostic(XlspSourceOpen *o,const XlspSourceDocument *documents,size_t count,size_t entry) {
    if(!o->failure.present||!o->failure.source.path)return XR_XIR_OK;
    for(size_t i=0;i<count;++i) {
        bool equal=false;char *path=NULL;
        XrXirStatus status=xlsp_source_uri_path(o->resources,documents[i].uri,documents[i].uri_length,&path);
        if(status==XR_XIR_OK)status=open_path_equal(o->resources,path,o->failure.source.path,&equal);
        xr_compile_resources_free(path);
        if(status!=XR_XIR_OK)return status;
        if(!equal)continue;
        const XlspSourceDocument *input=&documents[i];
        if(input->version<INT32_MIN||input->version>INT32_MAX||input->uri_length==SIZE_MAX)return XR_XIR_BAD_STRUCTURE;
        o->diagnostic_version=(int)input->version;
        if(i!=entry) {
            o->diagnostic_uri_length=input->uri_length;
            status=xlsp_source_buffer_new(o->resources,input->text,input->length,o->diagnostic_version,&o->diagnostic_buffer);
            if(status!=XR_XIR_OK)return status;
            status=open_status(xr_compile_resources_alloc(o->resources,input->uri_length+1,(void **)&o->diagnostic_uri));
            if(status!=XR_XIR_OK)return status;
            status=open_status(xr_compile_resources_work(o->resources,input->uri_length+1));
            if(status!=XR_XIR_OK)return status;
            memcpy(o->diagnostic_uri,input->uri,input->uri_length);o->diagnostic_uri[input->uri_length]=0;
        }
        o->diagnostic_resolved=true;break;
    }
    /* A closed dependency has no retained editor text. Keep its real failure,
     * but never convert its byte positions against the entry's source. */
    return XR_XIR_OK;
}
XrXirStatus xlsp_source_open_prepare(const XrXirCompileContext *context,const XlspSourceDocument *documents,size_t count,size_t entry,XlspSourceOpen **out) {
    if(!context||!context->resources||!documents||!count||entry>=count||!out||*out)return XR_XIR_BAD_STRUCTURE;
    const XlspSourceDocument *input=&documents[entry];
    if(!input->uri||input->uri_length==SIZE_MAX||input->version<INT32_MIN||input->version>INT32_MAX)return XR_XIR_BAD_STRUCTURE;
    XlspSourceOpen *o=NULL;XrCompilerSession *session=NULL;
    XrXirStatus status=open_status(xr_compile_resources_calloc(context->resources,1,sizeof(*o),(void **)&o));
    if(status!=XR_XIR_OK)return status;
    o->resources=context->resources;o->uri_length=input->uri_length;o->diagnostic_version=(int)input->version;
    status=xlsp_source_buffer_new(context->resources,input->text,input->length,(int)input->version,&o->buffer);
    if(status!=XR_XIR_OK)goto done;
    XrCompilerSessionStatus opened=xr_compile_session_new(context->resources,&session);
    if(opened!=XR_COMPILER_SESSION_OK){status=opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:opened==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;goto done;}
    XrParseStatus parsed=xlsp_source_syntax_build(session,input,&o->syntax);
    if(parsed!=XR_PARSE_OK&&parsed!=XR_PARSE_RECOVERED){status=parsed==XR_PARSE_BUDGET?XR_XIR_BUDGET:parsed==XR_PARSE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;goto done;}
    xr_compile_session_free(session);session=NULL;
    /* The syntax admission already validated the bounded URI, including NUL. */
    status=open_status(xr_compile_resources_alloc(context->resources,input->uri_length+1,(void **)&o->uri));
    if(status!=XR_XIR_OK)goto done;
    status=open_status(xr_compile_resources_work(context->resources,input->uri_length+1));
    if(status!=XR_XIR_OK)goto done;
    memcpy(o->uri,input->uri,input->uri_length);o->uri[input->uri_length]=0;
    unsigned stage=0;
    o->query_status=xlsp_source_workspace_build(context,documents,count,&o->workspace,&stage,&o->failure);
    status=o->query_status;
    if(status!=XR_XIR_OK) {
        bool language_status=status==XR_XIR_BAD_STRUCTURE||status==XR_XIR_BAD_TYPE||status==XR_XIR_BAD_VALUE||status==XR_XIR_UNRESOLVED||status==XR_XIR_UNSUPPORTED;
        if(!language_status||stage!=6||(!o->failure.present&&parsed!=XR_PARSE_RECOVERED))goto done;
        /* A genuine invalid editing state is owned, but no query is invented. */
        status=xlsp_source_syntax_diagnostics(o->syntax)?XR_XIR_OK:open_capture_diagnostic(o,documents,count,entry);
        if(status!=XR_XIR_OK)goto done;
    }
    *out=o;o=NULL;
done:
    xr_compile_session_free(session);xlsp_source_open_free(o);return status;
}
static bool open_member(XrJsonValue *object,const char *key,XrJsonValue *value) {
    if(!value)return false;
    int count=object->as.object.count;xjson_object_set_new(object,key,value);
    if(object->as.object.count==count){xjson_free(value);return false;}
    if(object->as.object.count!=count+1)return false;
    const XrJsonMember *m=&object->as.object.members[count];
    return m->key&&m->value==value&&m->key_len==strlen(key)&&!memcmp(m->key,key,m->key_len);
}
static XrJsonValue *open_position(XrLspPosition p) {
    XrJsonValue *value=xjson_new_object();if(!value)return NULL;
    if(!open_member(value,"line",xjson_new_number(p.line))||!open_member(value,"character",xjson_new_number(p.character))){xjson_free(value);return NULL;}
    return value;
}
static XrXirStatus open_add_diagnostic(XrJsonValue *array,XrLspPosition start,XrLspPosition end,const char *message) {
    XrJsonValue *diag=xjson_new_object(),*range=xjson_new_object();
    if(!diag||!range){xjson_free(diag);xjson_free(range);return XR_XIR_OUT_OF_MEMORY;}
    if(!open_member(range,"start",open_position(start))||!open_member(range,"end",open_position(end))){xjson_free(range);xjson_free(diag);return XR_XIR_OUT_OF_MEMORY;}
    if(!open_member(diag,"range",range)||!open_member(diag,"severity",xjson_new_number(1))||
        !open_member(diag,"source",xjson_new_string("xray"))||!open_member(diag,"message",xjson_new_string(message))) {xjson_free(diag);return XR_XIR_OUT_OF_MEMORY;}
    if(array->as.array.count==array->as.array.capacity&&array->as.array.capacity>INT_MAX/2){xjson_free(diag);return XR_XIR_BUDGET;}
    int count=array->as.array.count;xjson_array_push(array,diag);
    if(array->as.array.count!=count+1){xjson_free(diag);return XR_XIR_OUT_OF_MEMORY;}
    return XR_XIR_OK;
}
XrXirStatus xlsp_source_open_diagnostics_json(const XlspSourceOpen *o,const XlspSourceBuffer *buffer,const XlspSyntaxSnapshot *syntax,XrJsonValue **out) {
    if(!o||!buffer||!syntax||!out||*out)return XR_XIR_BAD_STRUCTURE;
    if(xlsp_source_syntax_diagnostics(syntax))return xlsp_source_syntax_diagnostics_json(syntax,out);
    XrJsonValue *array=xjson_new_array();if(!array)return XR_XIR_OUT_OF_MEMORY;
    XrXirStatus status=XR_XIR_OK;
    const char *text=xlsp_source_buffer_text(buffer);size_t length=xlsp_source_buffer_length(buffer);
    if(o->query_status!=XR_XIR_OK) {
        if(!o->failure.present||!o->diagnostic_resolved||o->failure.diagnostic.line<=0||o->failure.diagnostic.column<=0) {
            status=XR_XIR_UNRESOLVED;goto done;
        }
        if(o->diagnostic_buffer) {
            text=xlsp_source_buffer_text(o->diagnostic_buffer);length=xlsp_source_buffer_length(o->diagnostic_buffer);
        }
        XrLspPosition point={0};
        status=xlsp_source_position(o->resources,text,length,o->failure.diagnostic.line,o->failure.diagnostic.column,&point);
        if(status==XR_XIR_OK)status=open_add_diagnostic(array,point,point,o->failure.diagnostic.message);
        if(status!=XR_XIR_OK)goto done;
    }
    *out=array;array=NULL;
done:
    xjson_free(array);return status;
}
XR_FUNC XrXirStatus xlsp_source_open_diagnostics_params_json(const XlspSourceOpen *o,const XlspSourceBuffer *buffer,const XlspSyntaxSnapshot *syntax,XrJsonValue **out) {
    if(!o||!buffer||!syntax||!out||*out)return XR_XIR_BAD_STRUCTURE;
    bool imported=o->diagnostic_uri&&!xlsp_source_syntax_diagnostics(syntax);
    XrXirStatus status=open_status(xr_compile_resources_work(o->resources,imported?o->diagnostic_uri_length:o->uri_length));
    if(status==XR_XIR_OK)status=open_status(xr_compile_resources_work(o->resources,3*sizeof(XrJsonMember)+sizeof(XrJsonValue)));
    if(status!=XR_XIR_OK)return status;
    XrJsonValue *diagnostics=NULL;
    status=xlsp_source_open_diagnostics_json(o,buffer,syntax,&diagnostics);
    if(status!=XR_XIR_OK)return status;
    XrJsonValue *params=xjson_new_object();
    if(!params){xjson_free(diagnostics);return XR_XIR_OUT_OF_MEMORY;}
    if(!open_member(params,"uri",xjson_new_string(imported?o->diagnostic_uri:o->uri))||
        !open_member(params,"version",xjson_new_number(imported?o->diagnostic_version:xlsp_source_buffer_version(buffer)))) {
        xjson_free(diagnostics);xjson_free(params);return XR_XIR_OUT_OF_MEMORY;
    }
    if(!open_member(params,"diagnostics",diagnostics)){xjson_free(params);return XR_XIR_OUT_OF_MEMORY;}
    *out=params;return XR_XIR_OK;
}
