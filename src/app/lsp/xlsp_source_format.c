/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "xlsp_source_format.h"
#include "../../frontend/parser/xparse.h"
#include "../../shared/xr_utf8_core.h"
#include "../../xir/xxir_compile_memory.h"
#include <limits.h>
#include <string.h>

static XrXirStatus format_end(const XlspSourceBuffer *buffer,XrLspPosition *out) {
    int count=0;const uint32_t *lines=xlsp_source_buffer_lines(buffer,&count);uint32_t column=0;
    const char *text=xlsp_source_buffer_text(buffer);size_t length=xlsp_source_buffer_length(buffer);
    for(size_t i=lines[count-1];i<length;) {
        XrUtf8Step step=xr_utf8_core_decode_step((const uint8_t *)text+i,length-i);
        if(step.error!=XR_UTF8_OK||!step.scalar)return XR_XIR_BAD_STRUCTURE;
        uint32_t width=step.scalar>UINT32_C(0xffff)?2u:1u;
        if(column>INT32_MAX-width)return XR_XIR_BAD_STRUCTURE;column+=width;i+=step.consumed;
    }
    *out=(XrLspPosition){(uint32_t)(count-1),column};return XR_XIR_OK;
}
static bool format_member(XrJsonValue *object,const char *key,XrJsonValue *value) {
    if(!value)return false;int before=object->as.object.count;xjson_object_set_new(object,key,value);
    if(object->as.object.count==before){xjson_free(value);return false;}
    if(object->as.object.count!=before+1)return false;
    XrJsonMember *member=&object->as.object.members[before];
    return member->key&&member->value==value&&member->key_len==strlen(key)&&!memcmp(member->key,key,member->key_len);
}
static XrJsonValue *format_position(XrLspPosition position) {
    XrJsonValue *value=xjson_new_object();if(!value)return NULL;
    if(!format_member(value,"line",xjson_new_number(position.line))||
       !format_member(value,"character",xjson_new_number(position.character))){xjson_free(value);return NULL;}
    return value;
}
static XrJsonValue *format_range(XrLspPosition end) {
    XrJsonValue *value=xjson_new_object();if(!value)return NULL;
    if(!format_member(value,"start",format_position((XrLspPosition){0,0}))||
       !format_member(value,"end",format_position(end))){xjson_free(value);return NULL;}
    return value;
}
XR_FUNC XrXirStatus xlsp_source_format_json(XrCompileResources *resources,const XlspSourceDocument *document,
    const XrFmtConfig *configuration,XrJsonValue **out,XrCompileRequestControl *control) {
    if(!resources||!document||!document->uri||document->uri_length==SIZE_MAX||!configuration||!out||*out||
       document->version<INT32_MIN||document->version>INT32_MAX||configuration->indent_size<0||configuration->max_line_length<0)return XR_XIR_BAD_STRUCTURE;
    XlspSourceBuffer *buffer=NULL;XrCompilerSession *session=NULL;AstNode *ast=NULL;
    XrFmtOutput formatted={0};XrJsonValue *result=NULL,*edit=NULL;char *uri=NULL;
    XrXirStatus status=XR_XIR_OK;
    if(control){status=xir_compile_resource_status(xr_compile_request_control_poll(control));if(status!=XR_XIR_OK)goto done;}
    if(memchr(document->uri,0,document->uri_length)){status=XR_XIR_BAD_STRUCTURE;goto done;}
    status=xir_compile_resource_status(xr_compile_resources_alloc(resources,document->uri_length+1,(void **)&uri));if(status!=XR_XIR_OK)goto done;
    memcpy(uri,document->uri,document->uri_length);uri[document->uri_length]=0;
    status=xlsp_source_buffer_new(resources,document->text,document->length,(int)document->version,&buffer);if(status!=XR_XIR_OK)goto done;
    if(!document->length)goto publish;
    XrCompileResourceStatus opened=xr_compile_session_new(resources,&session);
    status=xir_compile_resource_status(opened);if(status!=XR_XIR_OK)goto done;
    if(control){status=xir_compile_resource_status(xr_compile_session_bind_request(session,control));if(status!=XR_XIR_OK)goto done;}
    XrParseStatus parsed=xr_compile_parse_with_trivia(session,xlsp_source_buffer_text(buffer),uri,NULL,&ast);
    if(parsed==XR_PARSE_SYNTAX)goto publish;
    if(parsed!=XR_PARSE_OK){
        switch(parsed) {
        case XR_PARSE_BUDGET: status=XR_XIR_BUDGET;break;
        case XR_PARSE_MEMORY_LIMIT: status=XR_XIR_MEMORY_LIMIT;break;
        case XR_PARSE_OVERFLOW: status=XR_XIR_OVERFLOW;break;
        case XR_PARSE_CANCELLED: status=XR_XIR_CANCELLED;break;
        case XR_PARSE_DEADLINE: status=XR_XIR_DEADLINE;break;
        case XR_PARSE_CLOCK_FAILURE: status=XR_XIR_CLOCK_FAILURE;break;
        case XR_PARSE_OUT_OF_MEMORY: status=XR_XIR_OUT_OF_MEMORY;break;
        default: status=XR_XIR_BAD_STRUCTURE;break;
        }
        goto done;
    }
    XrFmtStatus rendered=xr_compile_format_ast(xr_compile_session_compile_state(session),ast,configuration,&formatted);
    if(rendered!=XR_FMT_OK){
        switch(rendered) {
        case XR_FMT_BUDGET: status=XR_XIR_BUDGET;break;
        case XR_FMT_MEMORY_LIMIT: status=XR_XIR_MEMORY_LIMIT;break;
        case XR_FMT_OVERFLOW: status=XR_XIR_OVERFLOW;break;
        case XR_FMT_CANCELLED: status=XR_XIR_CANCELLED;break;
        case XR_FMT_DEADLINE: status=XR_XIR_DEADLINE;break;
        case XR_FMT_CLOCK_FAILURE: status=XR_XIR_CLOCK_FAILURE;break;
        case XR_FMT_OUT_OF_MEMORY: status=XR_XIR_OUT_OF_MEMORY;break;
        default: status=XR_XIR_BAD_STRUCTURE;break;
        }
        goto done;
    }
    xr_program_destroy(ast);ast=NULL;xr_compile_session_free(session);session=NULL;
    if(formatted.length==document->length&&!memcmp(formatted.text,xlsp_source_buffer_text(buffer),formatted.length))goto publish;
    XrLspPosition end={0};status=format_end(buffer,&end);if(status!=XR_XIR_OK)goto done;
    edit=xjson_new_object();if(!edit){status=XR_XIR_OUT_OF_MEMORY;goto done;}
    if(!format_member(edit,"range",format_range(end))||
       !format_member(edit,"newText",xjson_new_string_n(formatted.text,formatted.length))){status=XR_XIR_OUT_OF_MEMORY;goto done;}
publish:
    if(control){status=xir_compile_resource_status(xr_compile_request_control_poll(control));if(status!=XR_XIR_OK)goto done;}
    result=xjson_new_array();if(!result){status=XR_XIR_OUT_OF_MEMORY;goto done;}
    if(edit){xjson_array_push(result,edit);if(result->as.array.count!=1){status=XR_XIR_OUT_OF_MEMORY;goto done;}edit=NULL;}
    *out=result;result=NULL;
done:
    xjson_free(edit);xjson_free(result);xr_program_destroy(ast);xr_compile_session_free(session);
    xr_compile_format_output_free(&formatted);xlsp_source_buffer_free(buffer);xr_compile_resources_free(uri);return status;
}
