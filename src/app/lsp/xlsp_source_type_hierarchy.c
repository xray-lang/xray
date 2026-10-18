/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_type_hierarchy.c - Canonical type identities and owned hierarchy responses
 */
#include "xlsp_source_type_hierarchy.h"
#include "../../xir/xxir_interface.h"
#include "../../xir/xxir_implementation.h"
#include "../../xir/xxir_nominal.h"
#include "../../shared/xr_utf8_core.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
typedef struct TypeSymbol {
    size_t root,document;
    uint32_t declaration,module;
    XrXirSourceTypeKind kind;
    uint32_t table;
    XrLspRange range,selection;
} TypeSymbol;
typedef struct TypeWork {
    const XlspSourceHierarchyRequest *request;
    XrCompileResources *resources;
    XlspSourceBindings *bindings;
    TypeSymbol selected,*rows;
    size_t count,capacity;
    bool selected_ready;
    XrXirStatus status;
} TypeWork;
static bool type_fail(TypeWork *w,XrXirStatus status) {if(w->status==XR_XIR_OK)w->status=status;return false;}
static bool type_resource(TypeWork *w,XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK||type_fail(w,status==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE);
}
static bool type_work(TypeWork *w,uint64_t units) {
    return w->status==XR_XIR_OK&&type_resource(w,xr_compile_resources_work(w->resources,units));
}
static bool type_length(TypeWork *w,const char *text,size_t *out) {
    if(!text)return type_fail(w,XR_XIR_BAD_STRUCTURE);
    for(size_t i=0;;++i){if(!type_work(w,1))return false;if(!text[i]){*out=i;return true;}if(i==SIZE_MAX-1)return type_fail(w,XR_XIR_BUDGET);}
}
static bool type_equal(TypeWork *w,const char *a,const char *b,bool path,bool *out) {
    if(!a||!b)return type_fail(w,XR_XIR_BAD_STRUCTURE);
    for(;;){if(!type_work(w,2))return false;char x=*a++,y=*b++;
#ifdef XR_OS_WINDOWS
        if(path){if(x=='\\')x='/';if(y=='\\')y='/';}
#else
        (void)path;
#endif
        if(x!=y||!x){*out=x==y;return true;}}
}
static bool type_field(TypeWork *w,const XrJsonValue *object,const char *key,const XrJsonValue **out) {
    if(!object||object->type!=XR_JSON_OBJECT||object->as.object.count<0||
        (object->as.object.count&&!object->as.object.members))return type_fail(w,XR_XIR_BAD_STRUCTURE);
    size_t length=strlen(key);*out=NULL;
    for(int i=0;i<object->as.object.count;++i){if(!type_work(w,sizeof(XrJsonMember)))return false;
        const XrJsonMember *m=&object->as.object.members[i];if(m->key_len!=length)continue;
        if(!m->key||!type_work(w,length))return type_fail(w,XR_XIR_BAD_STRUCTURE);
        if(memcmp(m->key,key,length))continue;if(*out)return type_fail(w,XR_XIR_BAD_STRUCTURE);*out=m->value;}
    return *out!=NULL||type_fail(w,XR_XIR_BAD_STRUCTURE);
}
static bool type_json_text(TypeWork *w,const XrJsonValue *value,const char *text,size_t length) {
    if(!value||value->type!=XR_JSON_STRING||!value->as.string)return type_fail(w,XR_XIR_BAD_STRUCTURE);
    if(value->string_len!=length)return type_fail(w,XR_XIR_UNRESOLVED);
    if(!type_work(w,length))return false;return !memcmp(value->as.string,text,length)||type_fail(w,XR_XIR_UNRESOLVED);
}
static bool type_protocol_position(TypeWork *w,const XrJsonValue *object,XrLspPosition *out) {
    const XrJsonValue *line=NULL,*column=NULL;
    if(!type_field(w,object,"line",&line)||!type_field(w,object,"character",&column))return false;
    if(line->type!=XR_JSON_NUMBER||!line->is_integer||line->as.integer<0||line->as.integer>INT32_MAX||
        column->type!=XR_JSON_NUMBER||!column->is_integer||column->as.integer<0||column->as.integer>INT32_MAX)return type_fail(w,XR_XIR_BAD_STRUCTURE);
    *out=(XrLspPosition){(uint32_t)line->as.integer,(uint32_t)column->as.integer};return true;
}
static bool type_position_same(XrLspPosition a,XrLspPosition b){return a.line==b.line&&a.character==b.character;}
static bool type_range_same(XrLspRange a,XrLspRange b){return type_position_same(a.start,b.start)&&type_position_same(a.end,b.end);}
static int type_position_compare(XrLspPosition a,XrLspPosition b) {
    if(a.line!=b.line)return a.line<b.line?-1:1;if(a.character!=b.character)return a.character<b.character?-1:1;return 0;
}
static bool type_contains(XrLspRange range,XrLspPosition p) {
    return type_position_compare(p,range.start)>=0&&(type_position_compare(p,range.end)<0||
        (type_position_same(range.start,range.end)&&type_position_same(p,range.start)));
}
static int type_cursor_read(void *context,const uint8_t *address,uint8_t *out) {
    if(!type_work(context,1))return 0;*out=*address;return 1;
}
static bool type_cursor(TypeWork *w,XrLspPosition position) {
    const XlspSourceDocument *d=&w->request->documents[w->request->entry];uint32_t line=0,column=0;
    for(size_t offset=0;;){if(line==position.line&&column==position.character)return true;
        if(offset==d->length)return type_fail(w,XR_XIR_BAD_STRUCTURE);XrUtf8Step step;
        if(!xr_utf8_core_decode_step_read((const uint8_t *)d->text+offset,d->length-offset,type_cursor_read,w,&step))return false;
        if(step.error!=XR_UTF8_OK||!step.scalar)return type_fail(w,XR_XIR_BAD_STRUCTURE);
        if(step.scalar=='\r'||step.scalar=='\n'){
            if(line==position.line||line==UINT32_MAX)return type_fail(w,XR_XIR_BAD_STRUCTURE);
            if(step.scalar=='\r'&&offset+1<d->length){if(!type_work(w,1))return false;if(d->text[offset+1]=='\n')++step.consumed;}
            ++line;column=0;
        }else{uint32_t width=step.scalar>UINT32_C(0xffff)?2u:1u;
            if(column>INT32_MAX-width)return type_fail(w,XR_XIR_BAD_STRUCTURE);column+=width;
            if(line==position.line&&column>position.character)return type_fail(w,XR_XIR_BAD_STRUCTURE);}
        offset+=step.consumed;
    }
}
static const XrXirSourceView *type_view(TypeWork *w,size_t root){return xlsp_source_bindings_view(w->bindings,root);}
static bool type_span(TypeWork *w,size_t document,XrXirSourceRange source,XrLspRange *out) {
    const XlspSourceDocument *d=&w->request->documents[document];
    XrXirStatus s=xlsp_source_position(w->resources,d->text,d->length,source.line,source.column,&out->start);
    if(s==XR_XIR_OK)s=xlsp_source_position(w->resources,d->text,d->length,source.end_line,source.end_column,&out->end);
    return s==XR_XIR_OK||type_fail(w,s);
}
static bool type_document(TypeWork *w,const XrXirSourceView *view,uint32_t module,size_t *out) {
    if(module>=view->module_count)return type_fail(w,XR_XIR_BAD_STRUCTURE);
    for(size_t i=0;i<w->request->count;++i){bool same=false;
        if(!type_equal(w,view->modules[module].path,xlsp_source_bindings_path(w->bindings,i),true,&same))return false;
        if(same){*out=i;return true;}}
    return type_fail(w,XR_XIR_UNRESOLVED);
}
#include "xlsp_source_type_select.inc.c"
#include "xlsp_source_type_edges.inc.c"
#include "xlsp_source_type_json.inc.c"
#include "xlsp_source_type_request.inc.c"
