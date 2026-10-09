/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "xlsp_source_buffer.h"
#include "../../shared/xr_utf8_core.h"
#include <limits.h>
#include <string.h>
struct XlspSourceBuffer {XrCompileResources *resources;char *text;size_t length;int version,line_count;uint32_t *lines;};
static XrXirStatus buffer_status(XrCompileResourceStatus s) {
    return s==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:s==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        s==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
static int buffer_read(void *context,const uint8_t *address,uint8_t *out) {
    if(xr_compile_resources_work(context,1)!=XR_COMPILE_RESOURCE_OK)return 0;
    *out=*address;return 1;
}
static XrXirStatus buffer_step(XrCompileResources *resources,const char *text,size_t remaining,XrUtf8Step *step) {
    if(!xr_utf8_core_decode_step_read((const uint8_t *)text,remaining,buffer_read,resources,step))return XR_XIR_BUDGET;
    return step->error==XR_UTF8_OK&&step->scalar?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus buffer_validate(XrCompileResources *r,const char *text,size_t length) {
    if(!text||length==SIZE_MAX||length>UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
    for(size_t i=0;i<length;) {XrUtf8Step step;XrXirStatus status=buffer_step(r,text+i,length-i,&step);if(status!=XR_XIR_OK)return status;i+=step.consumed;}
    return XR_XIR_OK;
}
/* Locate an exact UTF-16 boundary. CRLF is one line break; neither a lone
 * surrogate half nor a column beyond its line is accepted as an edit cursor. */
static XrXirStatus buffer_offset(XrCompileResources *r,const char *text,size_t length,XrLspPosition target,size_t *out) {
    uint32_t row=0,column=0;size_t i=0;
    for(;;) {
        if(row==target.line&&column==target.character){*out=i;return XR_XIR_OK;}
        if(i==length||row>target.line||(row==target.line&&column>target.character))return XR_XIR_BAD_STRUCTURE;
        XrUtf8Step step;XrXirStatus status=buffer_step(r,text+i,length-i,&step);if(status!=XR_XIR_OK)return status;
        if(step.scalar=='\r'||step.scalar=='\n') {
            if(row==target.line)return XR_XIR_BAD_STRUCTURE;
            if(step.scalar=='\r'&&i+1<length) {
                if(xr_compile_resources_work(r,1)!=XR_COMPILE_RESOURCE_OK)return XR_XIR_BUDGET;
                if(text[i+1]=='\n')++step.consumed;
            }
            if(row==UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
            ++row;column=0;
        } else {
            uint32_t width=step.scalar>UINT32_C(0xffff)?2u:1u;
            if(column>UINT32_MAX-width)return XR_XIR_BAD_STRUCTURE;
            column+=width;
        }
        i+=step.consumed;
    }
}
static bool buffer_uinteger(const XrJsonValue *v,uint32_t *out) {
    if(!v||v->type!=XR_JSON_NUMBER||!v->is_integer||v->as.integer<0||v->as.integer>INT32_MAX)return false;
    *out=(uint32_t)v->as.integer;return true;
}
typedef struct BufferReader {XrCompileResources *resources;XrXirStatus status;} BufferReader;
static const XrJsonValue *buffer_field(BufferReader *reader,const XrJsonValue *v,const char *key) {
    if(reader->status!=XR_XIR_OK||!v||v->type!=XR_JSON_OBJECT)return NULL;
    size_t length=strlen(key);
    if(v->as.object.count<0||v->as.object.count>v->as.object.capacity||(v->as.object.count&&!v->as.object.members)){reader->status=XR_XIR_BAD_STRUCTURE;return NULL;}
    for(int i=0;i<v->as.object.count;++i) {
        const XrJsonMember *m=&v->as.object.members[i];
        reader->status=buffer_status(xr_compile_resources_work(reader->resources,sizeof(*m)));
        if(reader->status!=XR_XIR_OK)return NULL;
        if(m->key_len!=length)continue;
        reader->status=buffer_status(xr_compile_resources_work(reader->resources,length));
        if(reader->status!=XR_XIR_OK)return NULL;
        if(m->key&&!memcmp(m->key,key,length))return m->value;
    }
    return NULL;
}
static bool buffer_position(BufferReader *reader,const XrJsonValue *v,XrLspPosition *out) {
    return v&&v->type==XR_JSON_OBJECT&&buffer_uinteger(buffer_field(reader,v,"line"),&out->line)&&buffer_uinteger(buffer_field(reader,v,"character"),&out->character);
}
void xlsp_source_buffer_free(XlspSourceBuffer *b) {
    if(!b)return;xr_compile_resources_free(b->text);xr_compile_resources_free(b->lines);xr_compile_resources_free(b);
}
const char *xlsp_source_buffer_text(const XlspSourceBuffer *b){return b?b->text:NULL;}
size_t xlsp_source_buffer_length(const XlspSourceBuffer *b){return b?b->length:0;}
int xlsp_source_buffer_version(const XlspSourceBuffer *b){return b?b->version:0;}
const uint32_t *xlsp_source_buffer_lines(const XlspSourceBuffer *b,int *count){if(count)*count=b?b->line_count:0;return b?b->lines:NULL;}
static XrXirStatus buffer_index(XlspSourceBuffer *b) {
    size_t count=1;
    for(size_t i=0;i<b->length;++i) {
        if(xr_compile_resources_work(b->resources,1)!=XR_COMPILE_RESOURCE_OK)return XR_XIR_BUDGET;
        if(b->text[i]=='\r'||b->text[i]=='\n') {
            if(b->text[i]=='\r'&&i+1<b->length) {
                if(xr_compile_resources_work(b->resources,1)!=XR_COMPILE_RESOURCE_OK)return XR_XIR_BUDGET;
                if(b->text[i+1]=='\n')++i;
            }
            if(count==INT_MAX)return XR_XIR_BUDGET;++count;
        }
    }
    if(count>SIZE_MAX/sizeof(*b->lines))return XR_XIR_BUDGET;
    XrXirStatus status=buffer_status(xr_compile_resources_alloc(b->resources,count*sizeof(*b->lines),(void **)&b->lines));
    if(status!=XR_XIR_OK)return status;
    if(xr_compile_resources_work(b->resources,count*sizeof(*b->lines))!=XR_COMPILE_RESOURCE_OK)return XR_XIR_BUDGET;
    b->lines[0]=0;b->line_count=(int)count;count=1;
    for(size_t i=0;i<b->length;++i) {
        if(xr_compile_resources_work(b->resources,1)!=XR_COMPILE_RESOURCE_OK)return XR_XIR_BUDGET;
        if(b->text[i]=='\r'||b->text[i]=='\n') {
            if(b->text[i]=='\r'&&i+1<b->length) {
                if(xr_compile_resources_work(b->resources,1)!=XR_COMPILE_RESOURCE_OK)return XR_XIR_BUDGET;
                if(b->text[i+1]=='\n')++i;
            }
            b->lines[count++]=(uint32_t)(i+1);
        }
    }
    return XR_XIR_OK;
}
XrXirStatus xlsp_source_buffer_new(XrCompileResources *r,const char *text,size_t length,int version,XlspSourceBuffer **out) {
    if(!r||!out||*out)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=buffer_validate(r,text,length);if(status!=XR_XIR_OK)return status;
    XlspSourceBuffer *b=NULL;
    status=buffer_status(xr_compile_resources_calloc(r,1,sizeof(*b),(void **)&b));if(status!=XR_XIR_OK)return status;
    b->resources=r;b->length=length;b->version=version;
    status=buffer_status(xr_compile_resources_alloc(r,length+1,(void **)&b->text));
    if(status!=XR_XIR_OK)goto done;
    if(xr_compile_resources_work(r,length+1)!=XR_COMPILE_RESOURCE_OK){status=XR_XIR_BUDGET;goto done;}
    memcpy(b->text,text,length);b->text[length]=0;
    status=buffer_index(b);if(status==XR_XIR_OK){*out=b;b=NULL;}
done:
    xlsp_source_buffer_free(b);return status;
}
XrXirStatus xlsp_source_buffer_edit_json(XrCompileResources *r,const char *current,size_t length,int old_version,
    const XrJsonValue *changes,int version,XlspSourceBuffer **out) {
    if(!r||!out||*out||version<=old_version||!changes||changes->type!=XR_JSON_ARRAY||changes->as.array.count<0||changes->as.array.count>changes->as.array.capacity||(changes->as.array.count&&!changes->as.array.items))return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=buffer_validate(r,current,length);if(status!=XR_XIR_OK)return status;
    XlspSourceBuffer *b=NULL;
    status=buffer_status(xr_compile_resources_calloc(r,1,sizeof(*b),(void **)&b));if(status!=XR_XIR_OK)return status;
    b->resources=r;b->length=length;b->version=version;
    status=buffer_status(xr_compile_resources_alloc(r,length+1,(void **)&b->text));if(status!=XR_XIR_OK)goto done;
    if(xr_compile_resources_work(r,length+1)!=XR_COMPILE_RESOURCE_OK){status=XR_XIR_BUDGET;goto done;}
    memcpy(b->text,current,length);b->text[length]=0;
    for(int i=0;i<changes->as.array.count;++i) {
        if(xr_compile_resources_work(r,sizeof(XrJsonValue))!=XR_COMPILE_RESOURCE_OK){status=XR_XIR_BUDGET;goto done;}
        const XrJsonValue *change=changes->as.array.items[i];
        if(!change||change->type!=XR_JSON_OBJECT){status=XR_XIR_BAD_STRUCTURE;goto done;}
        BufferReader reader={r,XR_XIR_OK};
        const XrJsonValue *text=buffer_field(&reader,change,"text"),*range=buffer_field(&reader,change,"range"),*range_length=buffer_field(&reader,change,"rangeLength");
        if(reader.status!=XR_XIR_OK){status=reader.status;goto done;}
        if(!text||text->type!=XR_JSON_STRING){status=XR_XIR_BAD_STRUCTURE;goto done;}
        status=buffer_validate(r,text->as.string,text->string_len);if(status!=XR_XIR_OK)goto done;
        size_t start=0,end=b->length;
        if(range) {
            XrLspRange span;
            if(range->type!=XR_JSON_OBJECT||!buffer_position(&reader,buffer_field(&reader,range,"start"),&span.start)||!buffer_position(&reader,buffer_field(&reader,range,"end"),&span.end)) {status=reader.status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:reader.status;goto done;}
            status=buffer_offset(r,b->text,b->length,span.start,&start);if(status!=XR_XIR_OK)goto done;
            status=buffer_offset(r,b->text,b->length,span.end,&end);if(status!=XR_XIR_OK)goto done;
            if(end<start){status=XR_XIR_BAD_STRUCTURE;goto done;}
        } else if(range_length){status=XR_XIR_BAD_STRUCTURE;goto done;}
        if(range_length) {
            uint32_t declared=0;if(!buffer_uinteger(range_length,&declared)){status=XR_XIR_BAD_STRUCTURE;goto done;}
            uint64_t units=0;
            for(size_t at=start;at<end;) {
                XrUtf8Step step;status=buffer_step(r,b->text+at,end-at,&step);if(status!=XR_XIR_OK)goto done;
                units+=step.scalar>UINT32_C(0xffff)?2u:1u;at+=step.consumed;
            }
            if(units!=declared){status=XR_XIR_BAD_STRUCTURE;goto done;}
        }
        size_t kept=b->length-(end-start);
        if(text->string_len>UINT32_MAX-kept){status=XR_XIR_BAD_STRUCTURE;goto done;}
        size_t next_length=kept+text->string_len;char *next=NULL;
        status=buffer_status(xr_compile_resources_alloc(r,next_length+1,(void **)&next));if(status!=XR_XIR_OK)goto done;
        if(xr_compile_resources_work(r,next_length+1)!=XR_COMPILE_RESOURCE_OK){xr_compile_resources_free(next);status=XR_XIR_BUDGET;goto done;}
        memcpy(next,b->text,start);memcpy(next+start,text->as.string,text->string_len);memcpy(next+start+text->string_len,b->text+end,b->length-end);next[next_length]=0;
        xr_compile_resources_free(b->text);b->text=next;b->length=next_length;
    }
    status=buffer_index(b);
    if(status==XR_XIR_OK){*out=b;b=NULL;}
done:
    xlsp_source_buffer_free(b);return status;
}
