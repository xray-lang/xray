/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "xlsp_source_selection.h"
#include "../../shared/xr_utf8_core.h"
#include "../../base/xsimd.h"
#include <limits.h>
#include <string.h>

typedef struct SelectionReader {XrCompileResources *resources;XrXirStatus status;} SelectionReader;
static XrXirStatus selection_work(XrCompileResources *r,size_t units) {
    return xr_compile_resources_work(r,units)==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:XR_XIR_BUDGET;
}
static int selection_read(void *context,const uint8_t *address,uint8_t *out) {
    if(selection_work(context,1)!=XR_XIR_OK)return 0;*out=*address;return 1;
}
static const XrJsonValue *selection_field(SelectionReader *r,const XrJsonValue *object,const char *key) {
    if(r->status!=XR_XIR_OK||!object||object->type!=XR_JSON_OBJECT)return NULL;
    if(object->as.object.count<0||object->as.object.count>object->as.object.capacity||
       (object->as.object.count&&!object->as.object.members)){r->status=XR_XIR_BAD_STRUCTURE;return NULL;}
    size_t length=strlen(key);const XrJsonValue *found=NULL;
    for(int i=0;i<object->as.object.count;++i) {
        r->status=selection_work(r->resources,sizeof(XrJsonMember));if(r->status!=XR_XIR_OK)return NULL;
        const XrJsonMember *member=&object->as.object.members[i];
        if(!member->key||!member->value){r->status=XR_XIR_BAD_STRUCTURE;return NULL;}
        if(member->key_len!=length)continue;
        r->status=selection_work(r->resources,length);if(r->status!=XR_XIR_OK)return NULL;
        if(!memcmp(member->key,key,length)) {
            if(found){r->status=XR_XIR_BAD_STRUCTURE;return NULL;}found=member->value;
        }
    }
    return found;
}
static bool selection_uinteger(const XrJsonValue *value,uint32_t *out) {
    if(!value||value->type!=XR_JSON_NUMBER||!value->is_integer||value->as.integer<0||value->as.integer>INT32_MAX)return false;
    *out=(uint32_t)value->as.integer;return true;
}
static XrXirStatus selection_line(XrCompileResources *r,const XlspSourceBuffer *buffer,
    XrLspPosition cursor,XrLspRange *line,XrLspRange *word) {
    int count=0;const uint32_t *lines=xlsp_source_buffer_lines(buffer,&count);
    if(cursor.line>=(uint32_t)count)return XR_XIR_BAD_STRUCTURE;
    const char *text=xlsp_source_buffer_text(buffer);size_t begin=lines[cursor.line];
    size_t end=cursor.line+1<(uint32_t)count?lines[cursor.line+1]:xlsp_source_buffer_length(buffer);
    while(end>begin) {
        if(selection_work(r,1)!=XR_XIR_OK)return XR_XIR_BUDGET;
        if(text[end-1]!='\r'&&text[end-1]!='\n')break;--end;
    }
    bool boundary=false,inside=false;uint32_t column=0,word_begin=cursor.character,word_end=cursor.character,run_begin=0;
    for(size_t i=begin;i<end;) {
        if(column==cursor.character)boundary=true;
        XrUtf8Step step;
        if(!xr_utf8_core_decode_step_read((const uint8_t *)text+i,end-i,selection_read,r,&step))return XR_XIR_BUDGET;
        if(step.error!=XR_UTF8_OK||!step.scalar)return XR_XIR_BAD_STRUCTURE;
        uint32_t width=step.scalar>UINT32_C(0xffff)?2u:1u;
        if(column>INT32_MAX-width)return XR_XIR_BAD_STRUCTURE;
        if(selection_work(r,4)!=XR_XIR_OK)return XR_XIR_BUDGET;
        if(XR_IS_IDENT(text[i])) {
            if(!inside)run_begin=column;inside=true;
            if(cursor.character>=run_begin&&cursor.character<=column+width){word_begin=run_begin;word_end=column+width;}
        } else inside=false;
        column+=width;i+=step.consumed;
    }
    if(column==cursor.character)boundary=true;
    if(!boundary)return XR_XIR_BAD_STRUCTURE;
    *line=(XrLspRange){{cursor.line,0},{cursor.line,column}};
    *word=(XrLspRange){{cursor.line,word_begin},{cursor.line,word_end}};return XR_XIR_OK;
}
static bool selection_equal(XrLspRange a,XrLspRange b) {
    return a.start.line==b.start.line&&a.start.character==b.start.character&&
        a.end.line==b.end.line&&a.end.character==b.end.character;
}
static bool selection_member(XrJsonValue *object,const char *key,XrJsonValue *value) {
    if(!value)return false;int before=object->as.object.count;xjson_object_set_new(object,key,value);
    if(object->as.object.count==before){xjson_free(value);return false;}
    if(object->as.object.count!=before+1)return false;
    XrJsonMember *member=&object->as.object.members[before];
    return member->key&&member->value==value&&member->key_len==strlen(key)&&!memcmp(member->key,key,member->key_len);
}
static XrJsonValue *selection_position(XrLspPosition position) {
    XrJsonValue *value=xjson_new_object();if(!value)return NULL;
    if(!selection_member(value,"line",xjson_new_number(position.line))||
       !selection_member(value,"character",xjson_new_number(position.character))){xjson_free(value);return NULL;}
    return value;
}
/* Consumes parent even on failure, including a key-copy failure after adoption. */
static XrJsonValue *selection_node(XrLspRange range,XrJsonValue *parent) {
    XrJsonValue *value=xjson_new_object(),*span=NULL;if(!value)goto failed;
    span=xjson_new_object();if(!span)goto failed;
    if(!selection_member(span,"start",selection_position(range.start))||
       !selection_member(span,"end",selection_position(range.end)))goto failed;
    {XrJsonValue *moved=span;span=NULL;if(!selection_member(value,"range",moved))goto failed;}
    if(parent){XrJsonValue *moved=parent;parent=NULL;if(!selection_member(value,"parent",moved))goto failed;}
    return value;
failed:
    xjson_free(parent);xjson_free(span);xjson_free(value);return NULL;
}
XR_FUNC XrXirStatus xlsp_source_selection_json(XrCompileResources *resources,const char *text,size_t length,
    const XrJsonValue *positions,XrJsonValue **out) {
    if(!resources||!out||*out||!positions||positions->type!=XR_JSON_ARRAY||positions->as.array.count<0||
       positions->as.array.count>positions->as.array.capacity||(positions->as.array.count&&!positions->as.array.items))return XR_XIR_BAD_STRUCTURE;
    XlspSourceBuffer *buffer=NULL;XrJsonValue *result=NULL;
    XrXirStatus status=xlsp_source_buffer_new(resources,text,length,0,&buffer);if(status!=XR_XIR_OK)goto done;
    int count=0;(void)xlsp_source_buffer_lines(buffer,&count);XrLspRange last={0},ignored={0};
    status=selection_line(resources,buffer,(XrLspPosition){(uint32_t)(count-1),0},&last,&ignored);
    if(status!=XR_XIR_OK)goto done;
    XrLspRange document={{0,0},last.end};result=xjson_new_array();if(!result){status=XR_XIR_OUT_OF_MEMORY;goto done;}
    SelectionReader reader={resources,XR_XIR_OK};
    for(int i=0;i<positions->as.array.count;++i) {
        reader.status=selection_work(resources,sizeof(XrJsonValue *));if(reader.status!=XR_XIR_OK){status=reader.status;goto done;}
        const XrJsonValue *value=positions->as.array.items[i];XrLspPosition cursor={0};
        if(!selection_uinteger(selection_field(&reader,value,"line"),&cursor.line)||
           !selection_uinteger(selection_field(&reader,value,"character"),&cursor.character)) {
            status=reader.status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:reader.status;goto done;
        }
        XrLspRange line={0},word={0};status=selection_line(resources,buffer,cursor,&line,&word);if(status!=XR_XIR_OK)goto done;
        if(selection_work(resources,256)!=XR_XIR_OK){status=XR_XIR_BUDGET;goto done;}
        XrJsonValue *node=selection_node(document,NULL);if(!node){status=XR_XIR_OUT_OF_MEMORY;goto done;}
        if(!selection_equal(line,document)){node=selection_node(line,node);if(!node){status=XR_XIR_OUT_OF_MEMORY;goto done;}}
        if(!selection_equal(word,line)){node=selection_node(word,node);if(!node){status=XR_XIR_OUT_OF_MEMORY;goto done;}}
        int before=result->as.array.count;xjson_array_push(result,node);
        if(result->as.array.count==before){xjson_free(node);status=XR_XIR_OUT_OF_MEMORY;goto done;}
    }
    *out=result;result=NULL;
done:
    xjson_free(result);xlsp_source_buffer_free(buffer);return status;
}
