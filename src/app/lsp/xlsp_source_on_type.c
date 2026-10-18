/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_on_type.c - Owned lexical indentation edits
 *
 * KEY CONCEPT:
 *   Lex only the typed prefix so unfinished programs retain indentation, while
 *   a prefix ending inside a comment or literal cannot invent structural braces.
 */
#include "xlsp_source_on_type.h"
#include "../../frontend/lexer/xlex.h"
#include "../../toolchain/xcompiler_arena_backing.h"
#include "../../toolchain/xcompiler_session.h"
#include "../../shared/xr_utf8_core.h"
#include <limits.h>
#include <string.h>
typedef struct OnTypeWork {
    XrCompileResources *resources;
    const XlspSourceOnTypeInput *input;
    XlspSourceBuffer *buffer;
    const char *text;
    size_t begin,end,whitespace,cursor,previous,prefix;
    uint64_t indent;
    uint32_t line;
    bool edit;
} OnTypeWork;
static XrXirStatus on_type_resource(XrCompileResourceStatus s) {
    return s==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:s==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        s==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus on_type_work(OnTypeWork *w,uint64_t amount){return on_type_resource(xr_compile_resources_work(w->resources,amount));}
static int on_type_read(void *context,const uint8_t *address,uint8_t *out) {
    if(on_type_work(context,1)!=XR_XIR_OK)return 0;*out=*address;return 1;
}
static XrXirStatus on_type_line(OnTypeWork *w) {
    int count=0;const uint32_t *lines=xlsp_source_buffer_lines(w->buffer,&count);w->line=w->input->position.line;
    if(w->line>=(uint32_t)count)return XR_XIR_BAD_STRUCTURE;
    w->begin=lines[w->line];w->end=w->line+1<(uint32_t)count?lines[w->line+1]:xlsp_source_buffer_length(w->buffer);
    while(w->end>w->begin){XrXirStatus s=on_type_work(w,1);if(s!=XR_XIR_OK)return s;if(w->text[w->end-1]!='\r'&&w->text[w->end-1]!='\n')break;--w->end;}
    uint32_t column=0;
    for(size_t at=w->begin;;) {
        if(column==w->input->position.character){w->cursor=at;break;}if(at==w->end)return XR_XIR_BAD_STRUCTURE;
        XrUtf8Step step;if(!xr_utf8_core_decode_step_read((const uint8_t *)w->text+at,w->end-at,on_type_read,w,&step))return XR_XIR_BUDGET;
        if(step.error!=XR_UTF8_OK||!step.scalar)return XR_XIR_BAD_STRUCTURE;
        uint32_t width=step.scalar>UINT32_C(0xffff)?2u:1u;if(column>INT32_MAX-width)return XR_XIR_BAD_STRUCTURE;
        column+=width;if(column>w->input->position.character)return XR_XIR_BAD_STRUCTURE;at+=step.consumed;
    }
    w->whitespace=w->begin;
    while(w->whitespace<w->end){XrXirStatus s=on_type_work(w,1);if(s!=XR_XIR_OK)return s;char c=w->text[w->whitespace];if(c!=' '&&c!='\t')break;++w->whitespace;}
    if(w->whitespace-w->begin>INT32_MAX)return XR_XIR_BAD_STRUCTURE;
    w->previous=w->line?lines[w->line-1]:0;return XR_XIR_OK;
}
static XrXirStatus on_type_previous_indent(OnTypeWork *w) {
    for(size_t i=w->previous;i<w->begin;++i) {
        XrXirStatus s=on_type_work(w,1);if(s!=XR_XIR_OK)return s;char c=w->text[i];
        if(c==' ')++w->indent;else if(c=='\t')w->indent+=w->input->tab_size-w->indent%w->input->tab_size;else break;
        if(w->indent>INT32_MAX)return XR_XIR_BUDGET;
    }
    return XR_XIR_OK;
}
static XrXirStatus on_type_lex(OnTypeWork *w) {
    const XlspSourceOnTypeInput *input=w->input;
    if(input->trigger==';'||(input->trigger=='\n'&&(!w->line||w->cursor>w->whitespace))||
       (input->trigger=='}'&&(w->whitespace==w->end||w->text[w->whitespace]!='}'||w->cursor!=w->whitespace+1)))return XR_XIR_OK;
    w->prefix=input->trigger=='\n'?w->begin:w->cursor;
    char *prefix=NULL;XrCompilerSession *session=NULL;XrArena arena={0};
    XrXirStatus status=on_type_resource(xr_compile_resources_alloc(w->resources,w->prefix+1,(void **)&prefix));if(status!=XR_XIR_OK)goto done;
    status=on_type_work(w,w->prefix+1);if(status!=XR_XIR_OK)goto done;memcpy(prefix,w->text,w->prefix);prefix[w->prefix]=0;
    XrCompilerSessionStatus opened=xr_compile_session_new(w->resources,&session);
    if(opened!=XR_COMPILER_SESSION_OK){status=opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:opened==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;goto done;}
    XrCompileState *state=xr_compile_session_compile_state(session);XrArenaBacking backing;
    if(xr_compiler_arena_state_backing(state,&backing)!=XR_ARENA_OK){status=XR_XIR_BAD_STRUCTURE;goto done;}
    (void)xr_arena_open(&arena,XR_ARENA_SEGMENT_SIZE,&backing);status=on_type_resource(xr_compiler_arena_capture_status(&arena,state));if(status!=XR_XIR_OK)goto done;
    Scanner scanner={0};status=on_type_resource(xr_compile_scanner_open(&scanner,state,&arena,prefix));if(status!=XR_XIR_OK)goto done;
    uint64_t depth=0;bool unmatched=false,closing=false;Token last={0};size_t last_start=0;
    for(;;) {
        Token token=xr_scanner_scan(&scanner);status=on_type_resource(xr_compile_state_status(state));if(status!=XR_XIR_OK)goto done;
        status=on_type_work(w,sizeof(token));if(status!=XR_XIR_OK)goto done;
        if(token.type==TK_ERROR)goto done;if(token.type==TK_EOF)break;
        if(token.start<prefix||token.start>prefix+w->prefix||token.length<0||(size_t)token.length>(size_t)(prefix+w->prefix-token.start)){status=XR_XIR_BAD_STRUCTURE;goto done;}
        last=token;last_start=(size_t)(token.start-prefix);
        if(token.type==TK_LBRACE||token.type==TK_EMPTY_MAP_START){if(depth==INT32_MAX){status=XR_XIR_BUDGET;goto done;}++depth;}
        else if(token.type==TK_RBRACE){if(!depth)unmatched=true;else --depth;if(last_start==w->whitespace&&token.length==1)closing=true;}
    }
    if(unmatched)goto done;
    if(input->trigger=='}') {if(!closing)goto done;w->indent=depth*input->tab_size;}
    else {
        status=on_type_previous_indent(w);if(status!=XR_XIR_OK)goto done;
        if(last_start>=w->previous&&(last.type==TK_LBRACE||last.type==TK_EMPTY_MAP_START))w->indent+=input->tab_size;
    }
    if(w->indent>INT32_MAX){status=XR_XIR_BUDGET;goto done;}w->edit=true;
done:
    xr_arena_destroy(&arena);xr_compile_session_free(session);xr_compile_resources_free(prefix);return status;
}
static bool on_type_member(XrJsonValue *object,const char *key,XrJsonValue *child) {
    if(!child)return false;int before=object->as.object.count;xjson_object_set_new(object,key,child);
    if(object->as.object.count==before){xjson_free(child);return false;}if(object->as.object.count!=before+1)return false;
    XrJsonMember *m=&object->as.object.members[before];return m->key&&m->value==child&&m->key_len==strlen(key)&&!memcmp(m->key,key,m->key_len);
}
static XrJsonValue *on_type_position(uint32_t line,uint32_t column) {
    XrJsonValue *v=xjson_new_object();if(v&&(!on_type_member(v,"line",xjson_new_number(line))||!on_type_member(v,"character",xjson_new_number(column)))){xjson_free(v);v=NULL;}return v;
}
static XrJsonValue *on_type_range(uint32_t line,uint32_t end) {
    XrJsonValue *v=xjson_new_object();if(v&&(!on_type_member(v,"start",on_type_position(line,0))||!on_type_member(v,"end",on_type_position(line,end)))){xjson_free(v);v=NULL;}return v;
}
XR_FUNC XrXirStatus xlsp_source_on_type_json(XrCompileResources *resources,const XlspSourceDocument *document,
    const XlspSourceOnTypeInput *input,XrJsonValue **out) {
    if(!resources||!document||!document->uri||!input||!input->tab_size||input->tab_size>INT32_MAX||
       input->position.line>INT32_MAX||input->position.character>INT32_MAX||document->uri_length==SIZE_MAX||
       document->version<INT32_MIN||document->version>INT32_MAX||!out||*out||
       (input->trigger!='}'&&input->trigger!='\n'&&input->trigger!=';'))return XR_XIR_BAD_STRUCTURE;
    OnTypeWork w={0};w.resources=resources;w.input=input;char *indent=NULL;XrJsonValue *result=NULL,*edit=NULL;
    XrXirStatus status=on_type_work(&w,document->uri_length);if(status!=XR_XIR_OK)goto done;
    if(memchr(document->uri,0,document->uri_length)){status=XR_XIR_BAD_STRUCTURE;goto done;}
    status=xlsp_source_buffer_new(resources,document->text,document->length,(int)document->version,&w.buffer);if(status!=XR_XIR_OK)goto done;
    w.text=xlsp_source_buffer_text(w.buffer);status=on_type_line(&w);if(status!=XR_XIR_OK)goto done;
    status=on_type_lex(&w);if(status!=XR_XIR_OK)goto done;
    size_t length=0;
    if(w.edit) {
        size_t tabs=input->insert_spaces?0:(size_t)(w.indent/input->tab_size),spaces=(size_t)w.indent-tabs*input->tab_size;length=tabs+spaces;
        status=on_type_resource(xr_compile_resources_alloc(resources,length+1,(void **)&indent));if(status!=XR_XIR_OK)goto done;
        status=on_type_work(&w,length+1);if(status!=XR_XIR_OK)goto done;memset(indent,'\t',tabs);memset(indent+tabs,' ',spaces);indent[length]=0;
        status=on_type_work(&w,length);if(status!=XR_XIR_OK)goto done;
        if(length==w.whitespace-w.begin&&!memcmp(indent,w.text+w.begin,length))w.edit=false;
    }
    xlsp_source_buffer_free(w.buffer);w.buffer=NULL;w.text=NULL;
    result=xjson_new_array();if(!result){status=XR_XIR_OUT_OF_MEMORY;goto done;}
    if(w.edit) {
        status=on_type_work(&w,length+1);if(status!=XR_XIR_OK)goto done;
        edit=xjson_new_object();if(!edit||!on_type_member(edit,"range",on_type_range(w.line,(uint32_t)(w.whitespace-w.begin)))||
            !on_type_member(edit,"newText",xjson_new_string_n(indent,length))){status=XR_XIR_OUT_OF_MEMORY;goto done;}
        xjson_array_push(result,edit);if(result->as.array.count!=1){status=XR_XIR_OUT_OF_MEMORY;goto done;}edit=NULL;
    }
    *out=result;result=NULL;
done:
    xjson_free(edit);xjson_free(result);xr_compile_resources_free(indent);xlsp_source_buffer_free(w.buffer);return status;
}
