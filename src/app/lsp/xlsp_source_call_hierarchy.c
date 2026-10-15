/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_call_hierarchy.c - Project exact Source call facts into owned protocol values
 */
#include "xlsp_source_call_hierarchy.h"
#include "../../shared/xr_utf8_core.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
typedef struct CallSymbol {
    size_t root,document;
    uint32_t declaration,module;
    bool initializer;
    XrLspRange range,selection;
} CallSymbol;
typedef struct CallEdge {CallSymbol other;XrLspRange *sites;size_t count,capacity;} CallEdge;
typedef struct CallWork {
    const XlspSourceCallRequest *request;
    XrCompileResources *resources;
    XlspSourceBindings *bindings;
    CallSymbol selected;
    bool selected_ready;
    CallEdge *edges;
    size_t count,capacity;
    XrXirStatus status;
} CallWork;
static bool call_fail(CallWork *w,XrXirStatus status) {if(w->status==XR_XIR_OK)w->status=status;return false;}
static bool call_resource(CallWork *w,XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK||call_fail(w,status==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE);
}
static bool call_work(CallWork *w,uint64_t units) {
    return w->status==XR_XIR_OK&&call_resource(w,xr_compile_resources_work(w->resources,units));
}
static bool call_length(CallWork *w,const char *text,size_t *out) {
    if(!text)return call_fail(w,XR_XIR_BAD_STRUCTURE);
    for(size_t i=0;;++i){if(!call_work(w,1))return false;if(!text[i]){*out=i;return true;}if(i==SIZE_MAX-1)return call_fail(w,XR_XIR_BUDGET);}
}
static bool call_equal(CallWork *w,const char *a,const char *b,bool path,bool *out) {
    if(!a||!b)return call_fail(w,XR_XIR_BAD_STRUCTURE);
    for(;;){if(!call_work(w,2))return false;char x=*a++,y=*b++;
#ifdef XR_OS_WINDOWS
        if(path){if(x=='\\')x='/';if(y=='\\')y='/';}
#else
        (void)path;
#endif
        if(x!=y||!x){*out=x==y;return true;}}
}
static bool call_field(CallWork *w,const XrJsonValue *object,const char *key,const XrJsonValue **out) {
    if(!object||object->type!=XR_JSON_OBJECT||object->as.object.count<0||
        (object->as.object.count&&!object->as.object.members))return call_fail(w,XR_XIR_BAD_STRUCTURE);
    size_t length=strlen(key);*out=NULL;
    for(int i=0;i<object->as.object.count;++i){if(!call_work(w,sizeof(XrJsonMember)))return false;
        const XrJsonMember *m=&object->as.object.members[i];if(m->key_len!=length)continue;
        if(!m->key||!call_work(w,length))return call_fail(w,XR_XIR_BAD_STRUCTURE);
        if(memcmp(m->key,key,length))continue;if(*out)return call_fail(w,XR_XIR_BAD_STRUCTURE);*out=m->value;}
    return *out!=NULL||call_fail(w,XR_XIR_BAD_STRUCTURE);
}
static bool call_json_text(CallWork *w,const XrJsonValue *value,const char *text,size_t length) {
    if(!value||value->type!=XR_JSON_STRING||!value->as.string)return call_fail(w,XR_XIR_BAD_STRUCTURE);
    if(value->string_len!=length)return call_fail(w,XR_XIR_UNRESOLVED);
    if(!call_work(w,length))return false;return !memcmp(value->as.string,text,length)||call_fail(w,XR_XIR_UNRESOLVED);
}
static bool call_protocol_position(CallWork *w,const XrJsonValue *object,XrLspPosition *out) {
    const XrJsonValue *line=NULL,*column=NULL;
    if(!call_field(w,object,"line",&line)||!call_field(w,object,"character",&column))return false;
    if(line->type!=XR_JSON_NUMBER||!line->is_integer||line->as.integer<0||line->as.integer>INT32_MAX||
        column->type!=XR_JSON_NUMBER||!column->is_integer||column->as.integer<0||column->as.integer>INT32_MAX)return call_fail(w,XR_XIR_BAD_STRUCTURE);
    *out=(XrLspPosition){(uint32_t)line->as.integer,(uint32_t)column->as.integer};return true;
}
static bool call_position_same(XrLspPosition a,XrLspPosition b){return a.line==b.line&&a.character==b.character;}
static bool call_range_same(XrLspRange a,XrLspRange b){return call_position_same(a.start,b.start)&&call_position_same(a.end,b.end);}
static int call_position_compare(XrLspPosition a,XrLspPosition b) {
    if(a.line!=b.line)return a.line<b.line?-1:1;if(a.character!=b.character)return a.character<b.character?-1:1;return 0;
}
static bool call_contains(XrLspRange range,XrLspPosition p) {
    return call_position_compare(p,range.start)>=0&&(call_position_compare(p,range.end)<0||
        (call_position_same(range.start,range.end)&&call_position_same(p,range.start)));
}
static int call_cursor_read(void *context,const uint8_t *address,uint8_t *out) {
    if(!call_work(context,1))return 0;*out=*address;return 1;
}
static bool call_cursor(CallWork *w,XrLspPosition position) {
    const XlspSourceDocument *d=&w->request->documents[w->request->entry];uint32_t line=0,column=0;
    for(size_t offset=0;;){if(line==position.line&&column==position.character)return true;
        if(offset==d->length)return call_fail(w,XR_XIR_BAD_STRUCTURE);XrUtf8Step step;
        if(!xr_utf8_core_decode_step_read((const uint8_t *)d->text+offset,d->length-offset,call_cursor_read,w,&step))return false;
        if(step.error!=XR_UTF8_OK||!step.scalar)return call_fail(w,XR_XIR_BAD_STRUCTURE);
        if(step.scalar=='\r'||step.scalar=='\n'){
            if(line==position.line||line==UINT32_MAX)return call_fail(w,XR_XIR_BAD_STRUCTURE);
            if(step.scalar=='\r'&&offset+1<d->length){if(!call_work(w,1))return false;if(d->text[offset+1]=='\n')++step.consumed;}
            ++line;column=0;
        }else{uint32_t width=step.scalar>UINT32_C(0xffff)?2u:1u;
            if(column>INT32_MAX-width)return call_fail(w,XR_XIR_BAD_STRUCTURE);column+=width;
            if(line==position.line&&column>position.character)return call_fail(w,XR_XIR_BAD_STRUCTURE);}
        offset+=step.consumed;
    }
}
static const XrXirSourceView *call_view(CallWork *w,size_t root){return xlsp_source_bindings_view(w->bindings,root);}
static bool call_span(CallWork *w,size_t document,XrXirSourceRange source,XrLspRange *out) {
    const XlspSourceDocument *d=&w->request->documents[document];
    XrXirStatus s=xlsp_source_position(w->resources,d->text,d->length,source.line,source.column,&out->start);
    if(s==XR_XIR_OK)s=xlsp_source_position(w->resources,d->text,d->length,source.end_line,source.end_column,&out->end);
    return s==XR_XIR_OK||call_fail(w,s);
}
static bool call_document(CallWork *w,const XrXirSourceView *view,uint32_t module,size_t *out) {
    if(module>=view->module_count)return call_fail(w,XR_XIR_BAD_STRUCTURE);
    for(size_t i=0;i<w->request->count;++i){bool same=false;
        if(!call_equal(w,view->modules[module].path,xlsp_source_bindings_path(w->bindings,i),true,&same))return false;
        if(same){*out=i;return true;}}
    return call_fail(w,XR_XIR_UNRESOLVED);
}
static bool call_callable(const XrXirSourceDeclaration *d,const XrXirSourceDeclarationSyntax *syntax) {
    return d->kind==XR_XIR_SOURCE_FUNCTION||(d->kind==XR_XIR_SOURCE_MEMBER&&syntax->role==XR_XIR_SOURCE_SYNTAX_METHOD);
}
static bool call_target(CallWork *w,const XrXirSourceView *view,uint32_t *id) {
    for(uint32_t i=0;i<view->declaration_count;++i){if(!call_work(w,sizeof(XrXirSourceDeclaration)))return false;
        if(!*id||*id>view->declaration_count)return call_fail(w,XR_XIR_BAD_STRUCTURE);
        uint32_t next=view->declarations[*id-1].target;if(!next||next==*id)return true;*id=next;}
    return call_fail(w,XR_XIR_BAD_STRUCTURE);
}
static bool call_symbol(CallWork *w,size_t root,uint32_t id,CallSymbol *out) {
    const XrXirSourceView *view=call_view(w,root);const XrXirSourceSyntaxView *syntax=xlsp_source_bindings_syntax(w->bindings,root);
    const XrXirSourceCalls *calls=xlsp_source_bindings_calls(w->bindings,root);
    if(!id||id>view->declaration_count)return call_fail(w,XR_XIR_BAD_STRUCTURE);
    if(!call_work(w,sizeof(XrXirSourceDeclaration)+sizeof(XrXirSourceFunctionRange)))return false;
    const XrXirSourceDeclaration *d=&view->declarations[id-1];const XrXirSourceDeclarationSyntax *name=&syntax->declarations[id-1];
    if(!call_callable(d,name)||!calls->functions[id-1].known)return call_fail(w,XR_XIR_UNSUPPORTED);
    CallSymbol symbol={root,0,id,d->range.module,false,{0},{0}};
    if(!call_document(w,view,symbol.module,&symbol.document)||!call_span(w,symbol.document,calls->functions[id-1].range,&symbol.range))return false;
    if(name->role==XR_XIR_SOURCE_SYNTAX_CLOSURE)symbol.selection=(XrLspRange){symbol.range.start,symbol.range.start};
    else if((name->role!=XR_XIR_SOURCE_SYNTAX_NAME&&name->role!=XR_XIR_SOURCE_SYNTAX_METHOD)||
        !call_span(w,symbol.document,name->name,&symbol.selection))return call_fail(w,XR_XIR_UNSUPPORTED);
    *out=symbol;return true;
}
static bool call_module_symbol(CallWork *w,size_t root,uint32_t module,CallSymbol *out) {
    const XrXirSourceView *view=call_view(w,root);CallSymbol symbol={root,0,0,module,true,{0},{0}};
    if(!call_document(w,view,module,&symbol.document))return false;
    const XlspSourceDocument *d=&w->request->documents[symbol.document];int line=1,column=1;
    for(size_t i=0;i<d->length;++i){if(!call_work(w,1))return false;
        if(d->text[i]=='\n'){if(line==INT_MAX)return call_fail(w,XR_XIR_BUDGET);++line;column=1;}
        else {if(column==INT_MAX)return call_fail(w,XR_XIR_BUDGET);++column;}}
    if(!call_span(w,symbol.document,(XrXirSourceRange){module,1,1,line,column},&symbol.range))return false;
    *out=symbol;return true;
}
static bool call_symbol_same(CallWork *w,CallSymbol a,CallSymbol b,bool *out) {
    *out=false;if(a.initializer!=b.initializer)return true;
    const XrXirSourceView *va=call_view(w,a.root),*vb=call_view(w,b.root);bool same=false;
    if(!call_equal(w,va->modules[a.module].identity,vb->modules[b.module].identity,false,&same)||!same)return w->status==XR_XIR_OK;
    if(!call_work(w,2*sizeof(XrFingerprint)))return false;
    if(memcmp(&va->modules[a.module].fingerprint,&vb->modules[b.module].fingerprint,sizeof(XrFingerprint)))return call_fail(w,XR_XIR_UNRESOLVED);
    if(!call_range_same(a.range,b.range)||!call_range_same(a.selection,b.selection))return true;
    if(a.initializer){*out=true;return true;}
    const XrXirSourceDeclaration *da=&va->declarations[a.declaration-1],*db=&vb->declarations[b.declaration-1];
    if(da->kind!=db->kind)return true;
    const XrXirSourceSyntaxView *sa=xlsp_source_bindings_syntax(w->bindings,a.root),*sb=xlsp_source_bindings_syntax(w->bindings,b.root);
    if(sa->declarations[a.declaration-1].role!=sb->declarations[b.declaration-1].role)return true;
    if(sa->declarations[a.declaration-1].role==XR_XIR_SOURCE_SYNTAX_CLOSURE){*out=true;return true;}
    return call_equal(w,da->name,db->name,false,out);
}
static bool call_select(CallWork *w,CallSymbol symbol) {
    if(w->selected_ready){bool same=false;if(!call_symbol_same(w,w->selected,symbol,&same))return false;if(!same)return call_fail(w,XR_XIR_UNSUPPORTED);}
    else {w->selected=symbol;w->selected_ready=true;}return true;
}
#include "xlsp_source_call_select.inc.c"
#include "xlsp_source_call_edges.inc.c"
#include "xlsp_source_call_json.inc.c"
#include "xlsp_source_call_request.inc.c"
