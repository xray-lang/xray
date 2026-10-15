/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_binding_lenses.c - Complete CodeLens counts from Source bindings
 *
 * KEY CONCEPT:
 *   Published declaration and nominal kinds decide display eligibility, while
 *   resolved reference targets decide counts independently of semantic colors.
 */
#include "xlsp_source_binding_lenses.h"
#include "../../xir/xxir_types.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
typedef struct BindingLensRow {
    const char *identity,*name;
    XrXirSourceDeclarationKind kind;
    XrXirSourceRange source;
    XrLspRange range;
    uint32_t references;
} BindingLensRow;
typedef struct BindingLensSite {const char *identity;XrXirSourceRange range;} BindingLensSite;
typedef struct BindingLenses {
    XrCompileResources *resources;
    XlspSourceBindings *bindings;
    const XlspSourceDocument *documents;
    size_t count,entry,row_count,row_capacity;
    BindingLensRow *rows;
} BindingLenses;
static XrXirStatus binding_lens_resource(XrCompileResourceStatus s) {
    return s==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:s==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        s==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus binding_lens_work(BindingLenses *w,uint64_t n){return binding_lens_resource(xr_compile_resources_work(w->resources,n));}
static XrXirStatus binding_lens_equal(BindingLenses *w,const char *a,const char *b,bool path,bool *out) {
    if(!a||!b)return XR_XIR_UNRESOLVED;
    for(;;){XrXirStatus s=binding_lens_work(w,2);if(s!=XR_XIR_OK)return s;char x=*a++,y=*b++;
#ifdef XR_OS_WINDOWS
        if(path){if(x=='\\')x='/';if(y=='\\')y='/';}
#else
        (void)path;
#endif
        if(x!=y||!x){*out=x==y;return XR_XIR_OK;}}
}
static bool binding_lens_range_same(XrXirSourceRange a,XrXirSourceRange b) {
    return a.line==b.line&&a.column==b.column&&a.end_line==b.end_line&&a.end_column==b.end_column;
}
static XrXirStatus binding_lens_document(BindingLenses *w,const XrXirSourceView *view,uint32_t module,size_t *out) {
    if(module>=view->module_count)return XR_XIR_BAD_STRUCTURE;
    for(size_t i=0;i<w->count;++i){bool same=false;XrXirStatus s=binding_lens_equal(w,view->modules[module].path,xlsp_source_bindings_path(w->bindings,i),true,&same);if(s!=XR_XIR_OK)return s;if(same){*out=i;return XR_XIR_OK;}}
    return XR_XIR_UNRESOLVED;
}
static XrXirStatus binding_lens_eligible(BindingLenses *w,const XrXirSourceView *view,const XrXirSourceDeclaration *d,bool *out) {
    XrXirStatus s=binding_lens_work(w,sizeof(*d));if(s!=XR_XIR_OK)return s;*out=false;
    if(d->kind==XR_XIR_SOURCE_FUNCTION){*out=true;return XR_XIR_OK;}
    if(d->kind==XR_XIR_SOURCE_INTRINSIC&&d->signature&&*d->signature){*out=true;return XR_XIR_OK;}
    if(d->kind!=XR_XIR_SOURCE_TYPE||!d->type.known)return XR_XIR_OK;
    const XrXirTypeNode *node=xr_xir_type_node(view->types,d->type.type);if(!node||node->kind!=XR_XIR_TYPE_NOMINAL)return XR_XIR_OK;
    if(!view->types->nominals||!view->types->nominals->declarations||node->nominal.declaration>=view->types->nominals->count)return XR_XIR_BAD_STRUCTURE;
    uint32_t kind=view->types->nominals->declarations[node->nominal.declaration].kind;
    if(kind>XR_XIR_NOMINAL_CLASS)return XR_XIR_BAD_STRUCTURE;*out=kind==XR_XIR_NOMINAL_CLASS||kind==XR_XIR_NOMINAL_STRUCT;return XR_XIR_OK;
}
static XrXirStatus binding_lens_target(BindingLenses *w,const XrXirSourceView *view,uint32_t *id) {
    for(uint32_t n=0;n<view->declaration_count;++n) {
        XrXirStatus s=binding_lens_work(w,sizeof(uint32_t));if(s!=XR_XIR_OK)return s;
        if(!*id||*id>view->declaration_count)return XR_XIR_BAD_STRUCTURE;uint32_t next=view->declarations[*id-1].target;
        if(!next||next==*id)return XR_XIR_OK;*id=next;
    }
    return XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus binding_lens_key_same(BindingLenses *w,BindingLensRow a,BindingLensRow b,bool *out) {
    *out=false;if(a.kind!=b.kind||!binding_lens_range_same(a.source,b.source))return XR_XIR_OK;
    bool same=false;XrXirStatus s=binding_lens_equal(w,a.identity,b.identity,false,&same);if(s!=XR_XIR_OK||!same)return s;
    return binding_lens_equal(w,a.name,b.name,false,out);
}
static XrXirStatus binding_lens_name(BindingLenses *w,const XlspSourceDocument *text,const char *name,XrXirSourceRange source,XrLspRange *out) {
    if(!name||source.line<=0||source.column<=0||source.end_line!=source.line||source.end_column<=source.column)return XR_XIR_UNSUPPORTED;
    size_t length=0;for(;;++length){XrXirStatus s=binding_lens_work(w,1);if(s!=XR_XIR_OK)return s;if(!name[length])break;if(length==SIZE_MAX-1)return XR_XIR_BUDGET;}
    if(length!=(size_t)(source.end_column-source.column))return XR_XIR_UNSUPPORTED;
    size_t offset=0;int line=1;while(offset<text->length&&line<source.line){XrXirStatus s=binding_lens_work(w,1);if(s!=XR_XIR_OK)return s;if(text->text[offset++]=='\n')++line;}
    if(line!=source.line||(size_t)(source.column-1)>text->length-offset)return XR_XIR_UNSUPPORTED;offset+=(size_t)(source.column-1);
    if(length>text->length-offset)return XR_XIR_UNSUPPORTED;XrXirStatus s=binding_lens_work(w,length);if(s!=XR_XIR_OK)return s;
    if(memcmp(text->text+offset,name,length))return XR_XIR_UNSUPPORTED;
    s=xlsp_source_position(w->resources,text->text,text->length,source.line,source.column,&out->start);
    if(s==XR_XIR_OK)s=xlsp_source_position(w->resources,text->text,text->length,source.end_line,source.end_column,&out->end);return s;
}
static XrXirStatus binding_lens_row(BindingLenses *w,const XrXirSourceView *view,const XrXirSourceSyntaxView *syntax,uint32_t index,BindingLensRow *out) {
    XrXirStatus s=binding_lens_work(w,sizeof(*out));if(s!=XR_XIR_OK)return s;
    const XrXirSourceDeclaration *d=&view->declarations[index];const XrXirSourceDeclarationSyntax *fact=&syntax->declarations[index];
    if(fact->role==XR_XIR_SOURCE_SYNTAX_UNRESOLVED)return XR_XIR_UNSUPPORTED;
    if(fact->name.module>=view->module_count)return XR_XIR_BAD_STRUCTURE;
    *out=(BindingLensRow){view->modules[fact->name.module].identity,d->name,d->kind,fact->name,{0},0};return XR_XIR_OK;
}
static XrXirStatus binding_lens_append(BindingLenses *w,BindingLensRow row) {
    for(size_t i=0;i<w->row_count;++i){bool same=false;XrXirStatus s=binding_lens_key_same(w,w->rows[i],row,&same);if(s!=XR_XIR_OK)return s;if(same)return XR_XIR_OK;
        if(binding_lens_range_same(w->rows[i].source,row.source))return XR_XIR_UNSUPPORTED;}
    if(w->row_count==w->row_capacity){size_t next=w->row_capacity?w->row_capacity*2:16;if(next<w->row_capacity||next>SIZE_MAX/sizeof(row)||next>INT_MAX)return XR_XIR_BUDGET;
        XrXirStatus s=binding_lens_resource(xr_compile_resources_resize(w->resources,(void **)&w->rows,next*sizeof(row)));if(s!=XR_XIR_OK)return s;w->row_capacity=next;}
    size_t at=w->row_count;
    while(at){XrXirStatus s=binding_lens_work(w,sizeof(row));if(s!=XR_XIR_OK)return s;XrLspPosition before=w->rows[at-1].range.start;
        if(before.line<row.range.start.line||(before.line==row.range.start.line&&before.character<=row.range.start.character))break;w->rows[at]=w->rows[at-1];--at;}
    w->rows[at]=row;++w->row_count;return XR_XIR_OK;
}
static XrXirStatus binding_lens_declarations(BindingLenses *w) {
    for(size_t root=0;root<w->count;++root){const XrXirSourceView *view=xlsp_source_bindings_view(w->bindings,root);const XrXirSourceSyntaxView *syntax=xlsp_source_bindings_syntax(w->bindings,root);
        for(uint32_t i=0;i<view->declaration_count;++i){const XrXirSourceDeclaration *d=&view->declarations[i];bool eligible=false;XrXirStatus s=binding_lens_eligible(w,view,d,&eligible);if(s!=XR_XIR_OK)return s;if(!eligible)continue;
            const XrXirSourceDeclarationSyntax *fact=&syntax->declarations[i];if(fact->role==XR_XIR_SOURCE_SYNTAX_NONE||fact->role==XR_XIR_SOURCE_SYNTAX_CLOSURE||fact->role==XR_XIR_SOURCE_SYNTAX_RECEIVER)continue;
            size_t document=0;s=binding_lens_document(w,view,d->range.module,&document);if(s==XR_XIR_UNRESOLVED)continue;if(s!=XR_XIR_OK)return s;if(document!=w->entry)continue;
            if(fact->name.module!=d->range.module)return XR_XIR_BAD_STRUCTURE;
            BindingLensRow row={0};s=binding_lens_row(w,view,syntax,i,&row);if(s==XR_XIR_OK)s=binding_lens_name(w,&w->documents[document],row.name,row.source,&row.range);
            if(s==XR_XIR_OK)s=binding_lens_append(w,row);if(s!=XR_XIR_OK)return s;}}
    return XR_XIR_OK;
}
static XrXirStatus binding_lens_site(BindingLenses *w,BindingLensSite site,BindingLensSite **sites,size_t *count,size_t *capacity) {
    for(size_t i=0;i<*count;++i){XrXirStatus s=binding_lens_work(w,sizeof(site.range));if(s!=XR_XIR_OK)return s;if(!binding_lens_range_same((*sites)[i].range,site.range))continue;
        bool same=false;s=binding_lens_equal(w,(*sites)[i].identity,site.identity,false,&same);if(s!=XR_XIR_OK)return s;if(same)return XR_XIR_OK;}
    if(*count==INT_MAX)return XR_XIR_BUDGET;
    if(*count==*capacity){size_t next=*capacity?*capacity*2:16;if(next<*capacity||next>SIZE_MAX/sizeof(site)||next>INT_MAX)return XR_XIR_BUDGET;
        XrXirStatus s=binding_lens_resource(xr_compile_resources_resize(w->resources,(void **)sites,next*sizeof(site)));if(s!=XR_XIR_OK)return s;*capacity=next;}
    XrXirStatus s=binding_lens_work(w,sizeof(site));if(s==XR_XIR_OK)(*sites)[(*count)++]=site;return s;
}
static XrXirStatus binding_lens_count(BindingLenses *w,BindingLensRow *row) {
    BindingLensSite *sites=NULL;size_t count=0,capacity=0;XrXirStatus status=XR_XIR_OK;
    for(size_t root=0;root<w->count&&status==XR_XIR_OK;++root){const XrXirSourceView *view=xlsp_source_bindings_view(w->bindings,root);const XrXirSourceSyntaxView *syntax=xlsp_source_bindings_syntax(w->bindings,root);
        for(uint32_t i=0;i<view->reference_count;++i){const XrXirSourceReference *reference=&view->references[i];uint32_t id=reference->target?reference->target:reference->declaration;
            status=binding_lens_target(w,view,&id);if(status!=XR_XIR_OK)break;const XrXirSourceDeclaration *decl=&view->declarations[id-1];const XrXirSourceDeclarationSyntax *fact=&syntax->declarations[id-1];
            if(decl->kind!=row->kind||!binding_lens_range_same(row->source,fact->name))continue;
            if(fact->name.module>=view->module_count){status=XR_XIR_BAD_STRUCTURE;break;}
            BindingLensRow target={view->modules[fact->name.module].identity,decl->name,decl->kind,fact->name,{0},0};
            bool same=false;status=binding_lens_key_same(w,*row,target,&same);if(status!=XR_XIR_OK)break;if(!same)continue;
            if(fact->role==XR_XIR_SOURCE_SYNTAX_UNRESOLVED){status=XR_XIR_UNSUPPORTED;break;}
            XrXirSourceRange range=syntax->references[i];if(range.line<=0||range.column<=0||range.end_line<range.line||range.end_column<=0){status=XR_XIR_UNSUPPORTED;break;}
            size_t document=0;status=binding_lens_document(w,view,range.module,&document);if(status!=XR_XIR_OK)break;
            XrLspPosition start={0},end={0};const XlspSourceDocument *text=&w->documents[document];
            status=xlsp_source_position(w->resources,text->text,text->length,range.line,range.column,&start);
            if(status==XR_XIR_OK)status=xlsp_source_position(w->resources,text->text,text->length,range.end_line,range.end_column,&end);
            if(status!=XR_XIR_OK)break;if(end.line<start.line||(end.line==start.line&&end.character<=start.character)){status=XR_XIR_UNSUPPORTED;break;}
            status=binding_lens_site(w,(BindingLensSite){view->modules[range.module].identity,range},&sites,&count,&capacity);if(status!=XR_XIR_OK)break;}}
    if(status==XR_XIR_OK)row->references=(uint32_t)count;xr_compile_resources_free(sites);return status;
}
static bool binding_lens_member(XrJsonValue *object,const char *key,XrJsonValue *value) {
    if(!value)return false;int before=object->as.object.count;xjson_object_set_new(object,key,value);
    if(object->as.object.count==before){xjson_free(value);return false;}if(object->as.object.count!=before+1)return false;
    XrJsonMember *m=&object->as.object.members[before];return m->key&&m->value==value&&m->key_len==strlen(key)&&!memcmp(m->key,key,m->key_len);
}
static XrJsonValue *binding_lens_integer(uint32_t number){XrJsonValue *v=xjson_new_number(0);if(v){v->is_integer=true;v->as.integer=number;}return v;}
static XrJsonValue *binding_lens_position(XrLspPosition p) {
    XrJsonValue *v=xjson_new_object();if(v&&(!binding_lens_member(v,"line",binding_lens_integer(p.line))||!binding_lens_member(v,"character",binding_lens_integer(p.character)))){xjson_free(v);v=NULL;}return v;
}
static XrJsonValue *binding_lens_json(BindingLensRow row) {
    char title[64];int n=snprintf(title,sizeof(title),"%u reference%s",row.references,row.references==1?"":"s");if(n<=0||(size_t)n>=sizeof(title))return NULL;
    XrJsonValue *lens=xjson_new_object(),*range=xjson_new_object(),*command=xjson_new_object();if(!lens||!range||!command)goto failed;
    if(!binding_lens_member(range,"start",binding_lens_position(row.range.start))||!binding_lens_member(range,"end",binding_lens_position(row.range.end)))goto failed;
    if(!binding_lens_member(command,"title",xjson_new_string(title))||!binding_lens_member(command,"command",xjson_new_string("")))goto failed;
    {XrJsonValue *moved=range;range=NULL;if(!binding_lens_member(lens,"range",moved))goto failed;}
    {XrJsonValue *moved=command;command=NULL;if(!binding_lens_member(lens,"command",moved))goto failed;}return lens;
failed:
    xjson_free(range);xjson_free(command);xjson_free(lens);return NULL;
}
XR_FUNC XrXirStatus xlsp_source_binding_lenses_json(const XrXirCompileContext *context,const XlspSourceDocument *documents,size_t count,size_t entry,XrJsonValue **out) {
    if(!context||!context->resources||!documents||!count||entry>=count||!out||*out)return XR_XIR_BAD_STRUCTURE;
    BindingLenses w={0};w.resources=context->resources;w.documents=documents;w.count=count;w.entry=entry;XrJsonValue *result=NULL;
    XrXirStatus status=xlsp_source_bindings_build(context,documents,count,&w.bindings);if(status!=XR_XIR_OK)goto done;
    status=binding_lens_declarations(&w);if(status!=XR_XIR_OK)goto done;
    for(size_t i=0;i<w.row_count&&status==XR_XIR_OK;++i)status=binding_lens_count(&w,&w.rows[i]);if(status!=XR_XIR_OK)goto done;
    for(size_t i=0;i<w.row_count;++i){status=binding_lens_work(&w,sizeof(BindingLensRow));if(status!=XR_XIR_OK)goto done;
        BindingLensRow display={0};display.range=w.rows[i].range;display.references=w.rows[i].references;w.rows[i]=display;}
    xlsp_source_bindings_free(w.bindings);w.bindings=NULL;
    result=xjson_new_array();if(!result){status=XR_XIR_OUT_OF_MEMORY;goto done;}
    for(size_t i=0;i<w.row_count;++i){status=binding_lens_work(&w,sizeof(BindingLensRow)+64);if(status!=XR_XIR_OK)goto done;
        XrJsonValue *lens=binding_lens_json(w.rows[i]);if(!lens){status=XR_XIR_OUT_OF_MEMORY;goto done;}int before=result->as.array.count;xjson_array_push(result,lens);
        if(result->as.array.count==before){xjson_free(lens);status=XR_XIR_OUT_OF_MEMORY;goto done;}}
    *out=result;result=NULL;
done:
    xjson_free(result);xlsp_source_bindings_free(w.bindings);xr_compile_resources_free(w.rows);return status;
}
