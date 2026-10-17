/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_rename.c - Check exact binding edits before publishing versioned changes
 *
 * KEY CONCEPT:
 *   Canonical Source owns binding semantics. Consumers retain exact owned facts
 *   and reject unavailable evidence without publishing partial edits.
 */
#include "xlsp_source_rename.h"
#include "xlsp_source_bindings.h"
#include "xlsp_source_pattern_facts.h"
#include "../../frontend/lexer/xlex.h"
#include "../../toolchain/xcompiler_arena_backing.h"
#include "../../toolchain/xcompiler_session.h"
#include "../../base/xarena.h"
#include "../../shared/xr_utf8_core.h"
#include <limits.h>
#include <string.h>

typedef struct RenameEdit {
    size_t document;XrLspRange range;
    const char *preserved,*separator;
    bool prefix;
    char *replacement;
    size_t replacement_length;
    uint32_t replacement_width,name_offset;
} RenameEdit;
typedef struct RenameSymbol {RenameEdit declared;XrXirSourceDeclarationKind kind;const char *identity,*name;size_t identity_length,name_length;} RenameSymbol;
typedef struct RenameWork {
    XrCompileResources *resources;
    XlspSourceDocument *documents,*candidate_documents;
    XlspSourceBuffer **buffers,**candidates;
    bool *known_versions;
    size_t count,edit_count,definition;
    RenameEdit *edits;
    XrLspRange selected;
    char *old_name,*new_name;
    size_t old_length,new_length;
    uint32_t new_width;
    XlspSourceBindings *original,*candidate;
    RenameSymbol symbol;
    XrXirStatus json_status;
} RenameWork;
static XrXirStatus rename_resource(XrCompileResourceStatus s) {
    return s==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:s==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        s==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus rename_work(RenameWork *w,size_t amount) {return rename_resource(xr_compile_resources_work(w->resources,amount));}
static int rename_read(void *context,const uint8_t *address,uint8_t *out) {
    if(rename_work(context,1)!=XR_XIR_OK)return 0;*out=*address;return 1;
}
static XrXirStatus rename_copy(RenameWork *w,const char *text,size_t length,char **out) {
    if(!text||length==SIZE_MAX)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=rename_work(w,length);if(status!=XR_XIR_OK)return status;
    if(memchr(text,0,length))return XR_XIR_BAD_STRUCTURE;
    status=rename_resource(xr_compile_resources_alloc(w->resources,length+1,(void **)out));
    if(status==XR_XIR_OK)status=rename_work(w,length+1);
    if(status==XR_XIR_OK){memcpy(*out,text,length);(*out)[length]=0;}return status;
}
static XrXirStatus rename_equal(RenameWork *w,const char *a,size_t na,const char *b,size_t nb,bool *out) {
    *out=false;if(na!=nb)return XR_XIR_OK;XrXirStatus status=rename_work(w,na);
    if(status==XR_XIR_OK)*out=!memcmp(a,b,na);return status;
}
static void rename_release_sources(RenameWork *w) {
    for(size_t i=0;i<w->edit_count;++i)w->edits[i].preserved=NULL;
    xlsp_source_bindings_free(w->original);w->original=NULL;xlsp_source_bindings_free(w->candidate);w->candidate=NULL;
    if(w->buffers||w->candidates)for(size_t i=0;i<w->count;++i) {
        if(w->buffers){xlsp_source_buffer_free(w->buffers[i]);w->buffers[i]=NULL;}
        if(w->candidates){xlsp_source_buffer_free(w->candidates[i]);w->candidates[i]=NULL;}
    }
}
static void rename_free(RenameWork *w) {
    rename_release_sources(w);
    if(w->documents)for(size_t i=0;i<w->count;++i)xr_compile_resources_free((char *)w->documents[i].uri);
    xr_compile_resources_free(w->buffers);xr_compile_resources_free(w->candidates);
    xr_compile_resources_free(w->documents);xr_compile_resources_free(w->candidate_documents);
    xr_compile_resources_free(w->known_versions);
    for(size_t i=0;i<w->edit_count;++i)xr_compile_resources_free(w->edits[i].replacement);
    xr_compile_resources_free(w->edits);xr_compile_resources_free(w->old_name);xr_compile_resources_free(w->new_name);
}
static XrXirStatus rename_capture(RenameWork *w,const XlspSourceDocument *documents,size_t count) {
    w->count=count;XrXirStatus status=rename_resource(xr_compile_resources_calloc(w->resources,count,sizeof(*w->documents),(void **)&w->documents));
    if(status==XR_XIR_OK)status=rename_resource(xr_compile_resources_calloc(w->resources,count,sizeof(*w->buffers),(void **)&w->buffers));
    if(status==XR_XIR_OK)status=rename_resource(xr_compile_resources_calloc(w->resources,count,sizeof(*w->candidates),(void **)&w->candidates));
    if(status==XR_XIR_OK)status=rename_resource(xr_compile_resources_calloc(w->resources,count,sizeof(*w->candidate_documents),(void **)&w->candidate_documents));
    if(status!=XR_XIR_OK)return status;
    for(size_t i=0;i<count;++i) {
        const XlspSourceDocument *d=&documents[i];char *path=NULL,*uri=NULL;
        if(d->version<INT32_MIN||d->version>INT32_MAX)return XR_XIR_BAD_STRUCTURE;
        status=xlsp_source_uri_path(w->resources,d->uri,d->uri_length,&path);xr_compile_resources_free(path);if(status!=XR_XIR_OK)return status;
        status=rename_copy(w,d->uri,d->uri_length,&uri);
        w->documents[i]=(XlspSourceDocument){uri,NULL,d->uri_length,0,d->version};if(status!=XR_XIR_OK)return status;
        status=xlsp_source_buffer_new(w->resources,d->text,d->length,(int)d->version,&w->buffers[i]);if(status!=XR_XIR_OK)return status;
        status=rename_work(w,sizeof(*d));if(status!=XR_XIR_OK)return status;
        w->documents[i].text=xlsp_source_buffer_text(w->buffers[i]);w->documents[i].length=d->length;
    }
    return XR_XIR_OK;
}
static XrXirStatus rename_offset(RenameWork *w,size_t document,XrLspPosition position,size_t *out) {
    int count=0;const uint32_t *lines=xlsp_source_buffer_lines(w->buffers[document],&count);
    if(position.line>=(uint32_t)count)return XR_XIR_BAD_STRUCTURE;
    const char *text=w->documents[document].text;size_t begin=lines[position.line];
    size_t end=position.line+1<(uint32_t)count?lines[position.line+1]:w->documents[document].length;
    while(end>begin) {XrXirStatus status=rename_work(w,1);if(status!=XR_XIR_OK)return status;
        if(text[end-1]!='\r'&&text[end-1]!='\n')break;--end;}
    uint32_t column=0;
    for(size_t i=begin;;) {
        if(column==position.character){*out=i;return XR_XIR_OK;}if(i==end)return XR_XIR_BAD_STRUCTURE;
        XrUtf8Step step;if(!xr_utf8_core_decode_step_read((const uint8_t *)text+i,end-i,rename_read,w,&step))return XR_XIR_BUDGET;
        if(step.error!=XR_UTF8_OK||!step.scalar)return XR_XIR_BAD_STRUCTURE;
        uint32_t width=step.scalar>UINT32_C(0xffff)?2u:1u;if(column>INT32_MAX-width)return XR_XIR_BAD_STRUCTURE;
        column+=width;if(column>position.character)return XR_XIR_BAD_STRUCTURE;i+=step.consumed;
    }
}
static XrXirStatus rename_identifier(RenameWork *w,const char *name,size_t length) {
    if(!name||!length||length>INT32_MAX)return XR_XIR_BAD_STRUCTURE;
    XlspSourceBuffer *buffer=NULL;XrCompilerSession *session=NULL;XrArena arena={0};
    XrXirStatus status=xlsp_source_buffer_new(w->resources,name,length,0,&buffer);if(status!=XR_XIR_OK)goto done;
    XrCompilerSessionStatus opened=xr_compile_session_new(w->resources,&session);
    if(opened!=XR_COMPILER_SESSION_OK){status=opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:opened==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;goto done;}
    XrCompileState *state=xr_compile_session_compile_state(session);XrArenaBacking backing;
    if(xr_compiler_arena_state_backing(state,&backing)!=XR_ARENA_OK){status=XR_XIR_BAD_STRUCTURE;goto done;}
    (void)xr_arena_open(&arena,XR_ARENA_SEGMENT_SIZE,&backing);status=rename_resource(xr_compiler_arena_capture_status(&arena,state));if(status!=XR_XIR_OK)goto done;
    Scanner scanner={0};const char *text=xlsp_source_buffer_text(buffer);
    status=rename_resource(xr_compile_scanner_open(&scanner,state,&arena,text));if(status!=XR_XIR_OK)goto done;
    Token token=xr_scanner_scan(&scanner),end=xr_scanner_scan(&scanner);
    status=rename_resource(xr_compile_state_status(state));if(status!=XR_XIR_OK)goto done;
    if(token.type!=TK_NAME||token.start!=text||token.length!=(int)length||end.type!=TK_EOF){status=XR_XIR_BAD_STRUCTURE;goto done;}
    uint32_t width=0;
    for(size_t i=0;i<length;) {XrUtf8Step step;if(!xr_utf8_core_decode_step_read((const uint8_t *)text+i,length-i,rename_read,w,&step)){status=XR_XIR_BUDGET;goto done;}
        if(step.error!=XR_UTF8_OK||!step.scalar){status=XR_XIR_BAD_STRUCTURE;goto done;}
        uint32_t units=step.scalar>UINT32_C(0xffff)?2u:1u;if(width>INT32_MAX-units){status=XR_XIR_BAD_STRUCTURE;goto done;}width+=units;i+=step.consumed;}
    status=rename_copy(w,text,length,&w->new_name);if(status==XR_XIR_OK){w->new_length=length;w->new_width=width;}
done:
    xr_arena_destroy(&arena);xr_compile_session_free(session);xlsp_source_buffer_free(buffer);return status;
}
static bool rename_same(XrLspRange a,XrLspRange b) {
    return a.start.line==b.start.line&&a.start.character==b.start.character&&a.end.line==b.end.line&&a.end.character==b.end.character;
}
static int rename_order(RenameEdit a,RenameEdit b) {
    if(a.document!=b.document)return a.document<b.document?-1:1;
    if(a.range.start.line!=b.range.start.line)return a.range.start.line>b.range.start.line?-1:1;
    if(a.range.start.character!=b.range.start.character)return a.range.start.character>b.range.start.character?-1:1;return 0;
}
#include "xlsp_source_rename_facts.h"
static XrXirStatus rename_replacements(RenameWork *w) {
    for(size_t i=0;i<w->edit_count;++i) {
        RenameEdit *edit=&w->edits[i];if(!edit->preserved)continue;
        size_t kept=0;XrXirStatus status=rename_fact_length(w,edit->preserved,&kept);if(status!=XR_XIR_OK)return status;
        const char *separator=edit->separator?edit->separator:": ";size_t gap=strlen(separator);
        if(w->new_length>SIZE_MAX-gap-1||kept>SIZE_MAX-w->new_length-gap-1)return XR_XIR_BUDGET;
        size_t length=w->new_length+gap+kept;
        uint32_t width=edit->range.end.character-edit->range.start.character;
        if(w->new_width>INT32_MAX-gap||width>INT32_MAX-w->new_width-gap)return XR_XIR_BUDGET;
        status=rename_resource(xr_compile_resources_alloc(w->resources,length+1,(void **)&edit->replacement));if(status!=XR_XIR_OK)return status;
        status=rename_work(w,length+1);if(status!=XR_XIR_OK)return status;
        const char *first=edit->prefix?edit->preserved:w->new_name,*last=edit->prefix?w->new_name:edit->preserved;
        size_t first_length=edit->prefix?kept:w->new_length,last_length=edit->prefix?w->new_length:kept;
        memcpy(edit->replacement,first,first_length);memcpy(edit->replacement+first_length,separator,gap);
        memcpy(edit->replacement+first_length+gap,last,last_length);edit->replacement[length]=0;
        edit->replacement_length=length;edit->replacement_width=w->new_width+(uint32_t)gap+width;
        edit->name_offset=edit->prefix?width+(uint32_t)gap:0;edit->preserved=NULL;
    }
    return XR_XIR_OK;
}
static bool rename_member(XrJsonValue *object,const char *key,XrJsonValue *child) {
    if(!child)return false;int before=object->as.object.count;xjson_object_set_new(object,key,child);
    if(object->as.object.count==before){xjson_free(child);return false;}if(object->as.object.count!=before+1)return false;
    XrJsonMember *m=&object->as.object.members[before];return m->key&&m->value==child&&m->key_len==strlen(key)&&!memcmp(m->key,key,m->key_len);
}
static XrJsonValue *rename_integer(int64_t value) {XrJsonValue *v=xjson_new_number(0);if(v){v->is_integer=true;v->as.integer=value;}return v;}
static XrJsonValue *rename_json_position(XrLspPosition p) {
    XrJsonValue *v=xjson_new_object();if(v&&(!rename_member(v,"line",rename_integer(p.line))||!rename_member(v,"character",rename_integer(p.character)))){xjson_free(v);v=NULL;}return v;
}
static XrJsonValue *rename_json_range(XrLspRange range) {
    XrJsonValue *v=xjson_new_object();if(v&&(!rename_member(v,"start",rename_json_position(range.start))||!rename_member(v,"end",rename_json_position(range.end)))){xjson_free(v);v=NULL;}return v;
}
static bool rename_push(XrJsonValue *array,XrJsonValue *child) {
    if(!child)return false;if(array->as.array.count==array->as.array.capacity&&array->as.array.capacity>INT_MAX/2){xjson_free(child);return false;}
    int before=array->as.array.count;xjson_array_push(array,child);if(array->as.array.count!=before+1){xjson_free(child);return false;}return true;
}
static XrJsonValue *rename_json_edits(RenameWork *w,size_t document,const char *key) {
    XrJsonValue *array=xjson_new_array();if(!array)return NULL;
    for(size_t i=0;i<w->edit_count;++i) {
        w->json_status=rename_work(w,sizeof(RenameEdit));if(w->json_status!=XR_XIR_OK)goto failed;
        if(w->edits[i].document!=document)continue;
        const RenameEdit *item=&w->edits[i];const char *replacement=item->replacement?item->replacement:w->new_name;
        size_t length=item->replacement?item->replacement_length:w->new_length;
        w->json_status=rename_work(w,length+1);if(w->json_status!=XR_XIR_OK)goto failed;
        XrJsonValue *edit=xjson_new_object();if(!edit)goto failed;
        if(!rename_member(edit,"range",rename_json_range(w->edits[i].range))||
           !rename_member(edit,key,xjson_new_string_n(replacement,length))){xjson_free(edit);goto failed;}
        if(!rename_push(array,edit))goto failed;
    }
    return array;
failed:
    xjson_free(array);return NULL;
}
static XrXirStatus rename_apply(RenameWork *w) {
    for(size_t i=0;i<w->count;++i) {
        XrXirStatus status=rename_work(w,sizeof(XlspSourceDocument));if(status!=XR_XIR_OK)return status;
        w->candidate_documents[i]=w->documents[i];bool changed=false;
        for(size_t j=0;j<w->edit_count;++j){status=rename_work(w,1);if(status!=XR_XIR_OK)return status;if(w->edits[j].document==i){changed=true;break;}}
        if(!changed)continue;
        XrJsonValue *changes=rename_json_edits(w,i,"text");if(!changes)return w->json_status!=XR_XIR_OK?w->json_status:XR_XIR_OUT_OF_MEMORY;
        /* Draft versions are private. Published versions stay unchanged until
         * the client applies the versioned edit and sends a real notification. */
        status=xlsp_source_buffer_edit_json(w->resources,w->documents[i].text,w->documents[i].length,0,changes,1,&w->candidates[i]);xjson_free(changes);
        if(status!=XR_XIR_OK)return status;
        w->candidate_documents[i].text=xlsp_source_buffer_text(w->candidates[i]);w->candidate_documents[i].length=xlsp_source_buffer_length(w->candidates[i]);
    }
    return XR_XIR_OK;
}
static XrXirStatus rename_mapped(RenameWork *w,RenameEdit edit,XrLspRange *out) {
    int64_t column=(int64_t)edit.range.start.character+edit.name_offset;
    for(size_t i=0;i<w->edit_count;++i) {
        XrXirStatus status=rename_work(w,sizeof(RenameEdit));if(status!=XR_XIR_OK)return status;RenameEdit before=w->edits[i];
        if(before.document!=edit.document||before.range.start.line!=edit.range.start.line||
            rename_same(before.range,edit.range)||before.range.end.character>edit.range.start.character)continue;
        uint32_t width=before.replacement?before.replacement_width:w->new_width;
        column+=(int64_t)width-(before.range.end.character-before.range.start.character);
        if(column<0||column>INT32_MAX)return XR_XIR_BUDGET;
    }
    if(column>INT32_MAX-w->new_width)return XR_XIR_BUDGET;
    *out=(XrLspRange){{edit.range.start.line,(uint32_t)column},{edit.range.start.line,(uint32_t)column+w->new_width}};return XR_XIR_OK;
}
static XrXirStatus rename_check(RenameWork *w,const XrXirCompileContext *context) {
    XrXirStatus status=rename_replacements(w);if(status!=XR_XIR_OK)return status;
    status=rename_apply(w);if(status!=XR_XIR_OK)return status;
    status=xlsp_source_bindings_build(context,w->candidate_documents,w->count,&w->candidate);if(status!=XR_XIR_OK)return status;
    RenameEdit declared=w->edits[w->definition];XrLspRange expected={0};status=rename_mapped(w,declared,&expected);if(status!=XR_XIR_OK)return status;
    XlspSourceRenameInput selection={declared.document,expected.start,NULL,0,true,NULL};RenameSymbol symbol={0};
    status=rename_symbol_at(w,w->candidate,w->candidate_documents,&selection,&symbol);if(status!=XR_XIR_OK)return status;
    bool same=false;status=rename_equal(w,w->symbol.identity,w->symbol.identity_length,symbol.identity,symbol.identity_length,&same);if(status!=XR_XIR_OK)return status;
    if(!same||symbol.kind!=w->symbol.kind||symbol.declared.document!=declared.document||!rename_same(symbol.declared.range,expected))return XR_XIR_BAD_TYPE;
    status=rename_equal(w,symbol.name,symbol.name_length,w->new_name,w->new_length,&same);if(status!=XR_XIR_OK)return status;if(!same)return XR_XIR_BAD_TYPE;
    RenameEdit *actual=NULL;size_t count=0;status=rename_collect(w,w->candidate,w->candidate_documents,&symbol,&actual,&count);
    if(status!=XR_XIR_OK)goto done;if(count!=w->edit_count){status=XR_XIR_BAD_TYPE;goto done;}
    for(size_t i=0;i<w->edit_count;++i) {
        status=rename_mapped(w,w->edits[i],&expected);if(status!=XR_XIR_OK)goto done;bool found=false;
        for(size_t j=0;j<count;++j){status=rename_work(w,sizeof(RenameEdit));if(status!=XR_XIR_OK)goto done;
            if(actual[j].document==w->edits[i].document&&rename_same(actual[j].range,expected)){found=true;break;}}
        if(!found){status=XR_XIR_BAD_TYPE;goto done;}
    }
done:
    xr_compile_resources_free(actual);return status;
}
static XrJsonValue *rename_workspace_edit(RenameWork *w) {
    XrJsonValue *result=xjson_new_object(),*documents=xjson_new_array();if(!result||!documents)goto failed;
    for(size_t i=0;i<w->count;++i) {
        w->json_status=rename_work(w,sizeof(XlspSourceDocument));if(w->json_status!=XR_XIR_OK)goto failed;
        bool changed=false;for(size_t j=0;j<w->edit_count;++j){w->json_status=rename_work(w,sizeof(RenameEdit));if(w->json_status!=XR_XIR_OK)goto failed;if(w->edits[j].document==i){changed=true;break;}}if(!changed)continue;
        w->json_status=rename_work(w,w->documents[i].uri_length+1);if(w->json_status!=XR_XIR_OK)goto failed;
        XrJsonValue *document=xjson_new_object(),*identity=xjson_new_object();if(!document||!identity){xjson_free(document);xjson_free(identity);goto failed;}
        if(!rename_member(identity,"uri",xjson_new_string_n(w->documents[i].uri,w->documents[i].uri_length))||
           !rename_member(identity,"version",w->known_versions&& !w->known_versions[i]?xjson_new_null():rename_integer(w->documents[i].version))){xjson_free(identity);xjson_free(document);goto failed;}
        if(!rename_member(document,"textDocument",identity)||!rename_member(document,"edits",rename_json_edits(w,i,"newText"))){xjson_free(document);goto failed;}
        if(!rename_push(documents,document))goto failed;
    }
    {XrJsonValue *moved=documents;documents=NULL;if(!rename_member(result,"documentChanges",moved))goto failed;}return result;
failed:
    xjson_free(documents);xjson_free(result);return NULL;
}
XR_FUNC XrXirStatus xlsp_source_rename_json(const XrXirCompileContext *context,const XlspSourceDocument *documents,size_t count,
    const XlspSourceRenameInput *input,XrJsonValue **out) {
    if(!context||!context->resources||!documents||!count||count>SIZE_MAX/sizeof(XlspSourceDocument)||
       !input||input->entry>=count||input->position.line>INT32_MAX||input->position.character>INT32_MAX||!out||*out)return XR_XIR_BAD_STRUCTURE;
    RenameWork w={0};w.resources=context->resources;XrJsonValue *result=NULL;XrXirStatus status=XR_XIR_OK;
    if(!input->prepare){status=rename_identifier(&w,input->name,input->name_length);if(status!=XR_XIR_OK)goto done;}
    status=rename_capture(&w,documents,count);if(status!=XR_XIR_OK)goto done;
    if(input->known_versions) {
        status=rename_resource(xr_compile_resources_alloc(w.resources,count*sizeof(bool),(void **)&w.known_versions));if(status!=XR_XIR_OK)goto done;
        status=rename_work(&w,count*sizeof(bool));if(status!=XR_XIR_OK)goto done;memcpy(w.known_versions,input->known_versions,count*sizeof(bool));
    }
    size_t cursor=0;status=rename_offset(&w,input->entry,input->position,&cursor);if(status!=XR_XIR_OK)goto done;
    status=xlsp_source_bindings_build(context,w.documents,count,&w.original);if(status!=XR_XIR_OK)goto done;
    status=rename_bindings(&w,input);if(status!=XR_XIR_OK)goto done;
    if(!input->prepare) {
        bool same=false;status=rename_equal(&w,w.old_name,w.old_length,w.new_name,w.new_length,&same);if(status!=XR_XIR_OK)goto done;
        if(same)w.edit_count=0;else {status=rename_check(&w,context);if(status!=XR_XIR_OK)goto done;}
    }
    /* Only owned edit metadata and names remain during final JSON publication. */
    rename_release_sources(&w);
    if(input->prepare) {
        status=rename_work(&w,w.old_length+1);if(status!=XR_XIR_OK)goto done;
        result=xjson_new_object();if(result&&(!rename_member(result,"range",rename_json_range(w.selected))||
            !rename_member(result,"placeholder",xjson_new_string_n(w.old_name,w.old_length)))){xjson_free(result);result=NULL;}
    } else result=rename_workspace_edit(&w);
    if(!result){status=w.json_status!=XR_XIR_OK?w.json_status:XR_XIR_OUT_OF_MEMORY;goto done;}*out=result;result=NULL;
done:
    xjson_free(result);rename_free(&w);return status;
}
