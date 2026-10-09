/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "app/lsp/xlsp_source_buffer.h"
static const char buffer_original[]="A\xf0\x9f\x98\x80" "B\r\n\xe4\xb8\xad" "Z\n";
static const char buffer_expected[]="AQB\r\n\xe4\xb8\xad" "!\n";
static const char buffer_full_expected[]="x\n\xf0\x9f\x98\x80" "!\n";
static void buffer_json_member(XrJsonValue *object,const char *key,XrJsonValue *value) {
    CHECK(object&&value);int count=object->as.object.count;xjson_object_set_new(object,key,value);
    CHECK(object->as.object.count==count+1&&xjson_get(object,key)==value);
}
static XrJsonValue *buffer_integer(int value) {
    char text[32];int n=snprintf(text,sizeof(text),"%d",value);CHECK(n>0&&(size_t)n<sizeof(text));
    XrJsonValue *number=xjson_parse(text,(size_t)n);CHECK(number&&number->is_integer&&number->as.integer==value);return number;
}
static XrJsonValue *buffer_cursor(int line,int character) {
    XrJsonValue *p=xjson_new_object();CHECK(p);
    buffer_json_member(p,"line",buffer_integer(line));buffer_json_member(p,"character",buffer_integer(character));return p;
}
static XrJsonValue *buffer_edit(const char *text,int line,int begin,int end,int units) {
    XrJsonValue *edit=xjson_new_object();CHECK(edit);buffer_json_member(edit,"text",xjson_new_string(text));
    if(line>=0) {
        XrJsonValue *range=xjson_new_object();CHECK(range);
        buffer_json_member(range,"start",buffer_cursor(line,begin));buffer_json_member(range,"end",buffer_cursor(line,end));
        buffer_json_member(edit,"range",range);
        if(units>=0)buffer_json_member(edit,"rangeLength",buffer_integer(units));
    }
    return edit;
}
static void buffer_json_push(XrJsonValue *array,XrJsonValue *edit) {
    CHECK(array&&edit);int count=array->as.array.count;xjson_array_push(array,edit);CHECK(array->as.array.count==count+1);
}
static XrJsonValue *buffer_edits(unsigned mode) {
    XrJsonValue *changes=xjson_new_array();CHECK(changes);
    buffer_json_push(changes,mode?buffer_edit("x\n\xf0\x9f\x98\x80" "z\n",-1,0,0,-1):buffer_edit("Q",0,1,3,2));
    buffer_json_push(changes,buffer_edit("!",1,mode?2:1,mode?3:2,1));return changes;
}
static void buffer_facts(XlspSourceBuffer *b,unsigned mode) {
    CHECK(b&&xlsp_source_buffer_version(b)==9);
    const char *expected=mode?buffer_full_expected:buffer_expected;
    CHECK(xlsp_source_buffer_length(b)==strlen(expected)&&!strcmp(xlsp_source_buffer_text(b),expected));
    int count=0;const uint32_t *lines=xlsp_source_buffer_lines(b,&count);
    CHECK(lines&&count==3&&lines[0]==0&&lines[1]==(mode?2u:5u)&&lines[2]==(mode?8u:10u));
}
static XrXirStatus buffer_once(unsigned mode,XrCompileResourceLimits limits,XrCompileResourceStats *stats) {
    XrJsonValue *changes=buffer_edits(mode);char input[sizeof(buffer_original)];memcpy(input,buffer_original,sizeof(input));
    XrCompileResources *resources=NULL;XlspSourceBuffer *buffer=NULL;
    XrCompileResourceStatus created=xr_compile_resources_new(&limits,&resources);
    XrXirStatus status=created==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:created==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    if(status==XR_XIR_OK)status=xlsp_source_buffer_edit_json(resources,input,sizeof(input)-1,7,changes,9,&buffer);
    if(status==XR_XIR_OK)buffer_facts(buffer,mode);else CHECK(!buffer);
    if(stats)CHECK(xr_compile_resources_stats(resources,stats)==XR_COMPILE_RESOURCE_OK);
    xjson_free(changes);memset(input,'?',sizeof(input));xr_compile_resources_release(resources);
    CHECK(!lsp_json_live&&!lsp_json_bytes);
    if(status==XR_XIR_OK)buffer_facts(buffer,mode);
    xlsp_source_buffer_free(buffer);lsp_zero();return status;
}
static void buffer_rejections(void) {
    XrCompileResourceLimits limits=lsp_limits();XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrJsonValue *good=buffer_edits(0);XlspSourceBuffer *saved=NULL;
    CHECK(xlsp_source_buffer_edit_json(resources,buffer_original,sizeof(buffer_original)-1,7,good,9,&saved)==XR_XIR_OK);
    size_t before=source_fixture_compile_attempts;XlspSourceBuffer *occupied=saved;
    CHECK(xlsp_source_buffer_edit_json(resources,buffer_original,sizeof(buffer_original)-1,7,good,9,&occupied)==XR_XIR_BAD_STRUCTURE&&occupied==saved&&before==source_fixture_compile_attempts);
    XlspSourceBuffer *output=NULL;
    CHECK(xlsp_source_buffer_edit_json(resources,buffer_original,sizeof(buffer_original)-1,9,good,9,&output)==XR_XIR_BAD_STRUCTURE&&!output);
    CHECK(xlsp_source_buffer_edit_json(resources,buffer_original,sizeof(buffer_original)-1,9,good,8,&output)==XR_XIR_BAD_STRUCTURE&&!output);
    xjson_free(good);
    for(unsigned mode=0;mode<6;++mode) {
        XrJsonValue *changes=xjson_new_array();CHECK(changes);
        if(mode==0)buffer_json_push(changes,buffer_edit("Q",0,2,3,1)); /* Half surrogate. */
        if(mode==1)buffer_json_push(changes,buffer_edit("Q",0,1,3,1)); /* Actual UTF16 length is 2. */
        if(mode==2) {buffer_json_push(changes,buffer_edit("Q",0,1,3,2));buffer_json_push(changes,buffer_edit("!",1,99,100,1));} /* Later invalid edit rolls whole notification back. */
        if(mode==3)buffer_json_push(changes,buffer_edit("Q",0,3,1,-1));
        if(mode==4)buffer_json_push(changes,buffer_edit("\xff",-1,0,0,-1));
        if(mode==5) {XrJsonValue *edit=buffer_edit("abc",-1,0,0,-1);xjson_get(edit,"text")->as.string[1]=0;buffer_json_push(changes,edit);}
        CHECK(xlsp_source_buffer_edit_json(resources,buffer_original,sizeof(buffer_original)-1,7,changes,9,&output)==XR_XIR_BAD_STRUCTURE&&!output);
        buffer_facts(saved,0);xjson_free(changes);
    }
    /* Zero-change notification still publishes its newer version and exact text. */
    XrJsonValue *empty=xjson_new_array();CHECK(empty);
    CHECK(xlsp_source_buffer_edit_json(resources,xlsp_source_buffer_text(saved),xlsp_source_buffer_length(saved),9,empty,10,&output)==XR_XIR_OK);
    CHECK(xlsp_source_buffer_version(output)==10&&!strcmp(xlsp_source_buffer_text(output),buffer_expected));
    xjson_free(empty);xlsp_source_buffer_free(output);xlsp_source_buffer_free(saved);xr_compile_resources_release(resources);lsp_zero();CHECK(!lsp_json_live&&!lsp_json_bytes);
}
static void lsp_buffer_cases(void) {
    for(unsigned mode=0;mode<2;++mode) {
        size_t sites=0;XrCompileResourceStats required={0};
        for(size_t probe=0;probe<=sites;++probe) {
            source_fixture_compile_attempts=0;source_fixture_compile_fail_at=probe?probe-1:SIZE_MAX;source_fixture_compile_injected=false;
            XrXirStatus status=buffer_once(mode,lsp_limits(),probe?NULL:&required);
            if(!probe){CHECK(status==XR_XIR_OK);sites=source_fixture_compile_attempts;CHECK(sites);}
            else CHECK(status==XR_XIR_OUT_OF_MEMORY&&source_fixture_compile_injected);
        }
        source_fixture_compile_fail_at=SIZE_MAX;source_fixture_compile_injected=false;
        for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
            XrCompileResourceLimits limits={required.allocated_bytes,required.peak_bytes,required.work};
            uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
            CHECK(*bound&&*bound<UINT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
            CHECK(buffer_once(mode,limits,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));
        }
        printf("LSP edit mode=%u actual sites=%zu; original three axes; physical=0/0\n",mode,sites);
    }
    buffer_rejections();
}
