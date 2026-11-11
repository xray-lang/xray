/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_alias_spans.c - Real alias tokens survive parser and display owner death
 */
#include "xir/xxir_compile_memory.h"
#include "app/lsp/xlsp_source_index.h"
#include "toolchain/xcompiler_session.h"
#include "frontend/parser/xast_nodes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d:%s\n",__LINE__,#c);exit(1);}}while(0)
#define xr_compile_resources_new alias_resource_new_impl
#include "xir_sdk_resource_test.h"
#undef xr_compile_resources_new
static size_t alias_ledgers;
XR_FUNC XrCompileResourceStatus xr_compile_resources_new(const XrCompileResourceLimits *b,XrCompileResources **out) {
    CHECK(b&&b->memory_mode==XR_COMPILE_MEMORY_FINITE&&b->memory_limit<=UINT64_C(8)*1024*1024&&b->profile_allocations);
    ++alias_ledgers;return alias_resource_new_impl(b,out);
}
#include "lsp_open_json_owner.h"
typedef struct AliasCase {
    const char *text,*name;
    int line,column,end_line,end_column;
    uint32_t utf_column,utf_end_column;
    bool exported,recovered,generic;
    uint32_t utf_line,utf_end_line;
} AliasCase;
static const AliasCase cases[]={
    {"type Count = i64","Count",1,6,1,17,5,16,false,false,false,0,0},
    {"/* \xf0\x9f\x98\x80 */ export type Count = i64","Count",1,24,1,35,21,32,true,false,false,0,0},
    {"export type Pair<T> = {left:T,right:T}","Pair",1,13,1,39,12,38,true,false,true,0,0},
    {"type\r\n  Counter =\r\n    i64\r\n","Counter",2,3,3,8,2,7,false,false,false,1,2},
    {"/* alias */\rtype Count=i64\r","Count",1,18,1,27,5,14,false,false,false,1,1},
    {"type /* middle */ Count = i64\n","Count",1,19,1,30,18,29,false,false,false,0,0},
    {"type Count = i64\nvar *** = ;\n","Count",1,6,1,17,5,16,false,true,false,0,0},
    {"type Mapper<T> = fn(T)->T","Mapper",1,6,1,26,5,25,false,false,true,0,0},
    {"// alias\rtype Count=i64\r",NULL,0,0,0,0,0,0,false,false,false,0,0}
};
static const char alias_uri[]="file:///E:/alias-source-owner/sample.xr";
static const XrCompileResourceLimits limits={XR_COMPILE_MEMORY_FINITE,UINT64_C(8)*1024*1024,true};
static XrXirStatus resource_status(XrCompileResourceStatus s) {
    return xir_compile_resource_status(s);
}
static XrXirStatus parse_status(XrParseStatus status) {
    switch(status) {
    case XR_PARSE_OK: case XR_PARSE_RECOVERED: return XR_XIR_OK;
    case XR_PARSE_BUDGET: return XR_XIR_BUDGET;
    case XR_PARSE_MEMORY_LIMIT: return XR_XIR_MEMORY_LIMIT;
    case XR_PARSE_OVERFLOW: return XR_XIR_OVERFLOW;
    case XR_PARSE_CANCELLED: return XR_XIR_CANCELLED;
    case XR_PARSE_DEADLINE: return XR_XIR_DEADLINE;
    case XR_PARSE_CLOCK_FAILURE: return XR_XIR_CLOCK_FAILURE;
    case XR_PARSE_OUT_OF_MEMORY: return XR_XIR_OUT_OF_MEMORY;
    default: return XR_XIR_BAD_STRUCTURE;
    }
}
static void syntax_facts(const XlspSyntaxSnapshot *syntax,const AliasCase *c) {
    CHECK(xlsp_source_syntax_status(syntax)==(c->recovered?XR_PARSE_RECOVERED:XR_PARSE_OK));
    const AstNode *root=xlsp_source_syntax_ast(syntax);CHECK(root&&root->type==AST_PROGRAM);unsigned found=0;
    for(int i=0;i<root->as.program.count;++i){const AstNode *node=root->as.program.statements[i];
        if(!node||node->type!=AST_TYPE_ALIAS)continue;
        ++found;
        CHECK(!strcmp(node->as.type_alias.name,c->name)&&node->line==c->line&&node->column==c->column);
        CHECK(node->end_line==c->end_line&&node->end_column==c->end_column&&node->is_exported==c->exported);
        CHECK(node->as.type_alias.resolved_type&&node->as.type_alias.type_param_count==(c->generic?1:0));
    }
    CHECK(found==(c->name?1u:0u));
    if(!c->name)CHECK(root->as.program.count==0);
}
static void selection(XrLspRange r,const AliasCase *c) {
    CHECK(r.start.line==c->utf_line&&r.end.line==r.start.line&&r.start.character==c->utf_column&&r.end.character==c->utf_column+strlen(c->name));
}
static void index_facts(const XlspSourceIndex *index,const AliasCase *c) {
    size_t count=0;const XlspSourceIndexSymbol *s=xlsp_source_index_symbols(index,&count);
    if(!c->name){CHECK(!count&&xlsp_source_index_parse_status(index)==XR_PARSE_OK);return;}
    CHECK(count==1&&s);
    CHECK(!strcmp(s[0].name,c->name)&&(uint32_t)s[0].kind==26&&s[0].exported==c->exported);selection(s[0].selection,c);
    CHECK(!strcmp(xlsp_source_index_uri(index),alias_uri)&&xlsp_source_index_version(index)==19);
    CHECK(xlsp_source_index_parse_status(index)==(c->recovered?XR_PARSE_RECOVERED:XR_PARSE_OK));
}
static void outline_facts(const XlspSourceOutline *outline,const AliasCase *c,XrJsonValue *json) {
    size_t count=0;const XlspSourceSymbol *s=xlsp_source_outline_symbols(outline,&count);
    if(!c->name){CHECK(!count&&json&&json->type==XR_JSON_ARRAY&&!json->as.array.count);return;}
    CHECK(count==1&&s);
    CHECK(!strcmp(s[0].name,c->name)&&s[0].kind==26&&s[0].parent==UINT32_MAX);selection(s[0].selection,c);
    CHECK(s[0].range.start.line==c->utf_line&&s[0].range.start.character==c->utf_column);
    CHECK(s[0].range.end.line==c->utf_end_line&&s[0].range.end.character==c->utf_end_column);
    CHECK(json&&json->type==XR_JSON_ARRAY&&json->as.array.count==1);
    XrJsonValue *item=xjson_array_get(json,0),*r=xjson_get_object(item,"selectionRange"),*start=xjson_get_object(r,"start");
    CHECK(!strcmp(xjson_get_string(item,"name"),c->name)&&xjson_get_int(item,"kind")==26);
    CHECK(xjson_get_int(start,"line")==c->utf_line&&xjson_get_int(start,"character")==c->utf_column);
}
static XrXirStatus alias_once(unsigned method,unsigned mode,XrCompileResourceLimits bounds,XrCompileResourceStats *stats) {
    CHECK(method<2&&mode<9);const AliasCase *c=&cases[mode];char text[256],uri[sizeof(alias_uri)];strcpy(text,c->text);memcpy(uri,alias_uri,sizeof(uri));
    XrCompileResources *r=NULL;XrCompilerSession *session=NULL;XlspSyntaxSnapshot *syntax=NULL;XlspSourceIndex *index=NULL;XlspSourceOutline *outline=NULL;XrJsonValue *json=NULL;
    XlspSourceDocument input={uri,text,sizeof(uri)-1,strlen(text),19};XrXirStatus s=resource_status(xr_compile_resources_new(&bounds,&r));
    if(s!=XR_XIR_OK)goto done;
    if(!method)s=xlsp_source_index_build(r,&input,&index);
    else{
        XrCompileResourceStatus opened=xr_compile_session_new(r,&session);
        s=xir_compile_resource_status(opened);
        if(s==XR_XIR_OK)s=parse_status(xlsp_source_syntax_build(session,&input,&syntax));
        if(s==XR_XIR_OK){syntax_facts(syntax,c);s=xlsp_source_syntax_outline(syntax,&outline);}
        xlsp_source_syntax_free(syntax);syntax=NULL;xr_compile_session_free(session);session=NULL;
        memset(text,'?',sizeof(text));memset(uri,'?',sizeof(uri));
        if(s==XR_XIR_OK)s=xlsp_source_outline_json(outline,&json);
    }
done:
    if(stats)*stats=r?sdk_stats(r):(XrCompileResourceStats){0};
    memset(text,'?',sizeof(text));memset(uri,'?',sizeof(uri));memset(&input,0,sizeof(input));
    xlsp_source_syntax_free(syntax);xr_compile_session_free(session);xr_compile_resources_release(r);
    if(s==XR_XIR_OK){if(!method)index_facts(index,c);else outline_facts(outline,c,json);}
    else CHECK(!index&&!json);
    xlsp_source_index_free(index);xlsp_source_outline_free(outline);xjson_free(json);
    CHECK(alias_ledgers==1&&!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes);return s;
}
#include "alias_resources.h"
static XrXirStatus rejected_once(unsigned mode,XrCompileResourceLimits bounds,XrCompileResourceStats *stats) {
    const char *texts[]={"type Pair<T,T> = {a:T}\n","type Pair<T:Sendable> = {a:T}\n","type Loop = Loop\n","type Mapper<T> = (T)->T"};
    const char *messages[]={"duplicate type parameter","do not support constraints","circular type alias","function types are written"};
    CHECK(mode<4);XrCompileResources *r=NULL;XrCompilerSession *session=NULL;XlspSyntaxSnapshot *syntax=NULL;
    XrXirStatus s=resource_status(xr_compile_resources_new(&bounds,&r));
    if(s==XR_XIR_OK){XrCompileResourceStatus opened=xr_compile_session_new(r,&session);
        s=xir_compile_resource_status(opened);}
    if(s==XR_XIR_OK){XlspSourceDocument input={alias_uri,texts[mode],sizeof(alias_uri)-1,strlen(texts[mode]),19};
        XrParseStatus parsed=xlsp_source_syntax_build(session,&input,&syntax);s=parse_status(parsed);
        if(s==XR_XIR_OK)CHECK(parsed==XR_PARSE_RECOVERED);}
    if(stats)*stats=r?sdk_stats(r):(XrCompileResourceStats){0};
    xr_compile_session_free(session);
    xr_compile_resources_release(r);
    if(s==XR_XIR_OK){bool found=false;
        for(const XlspSyntaxDiagnostic *d=xlsp_source_syntax_diagnostics(syntax);d;d=d->next)if(strstr(d->message,messages[mode]))found=true;
        CHECK(found);
    }else CHECK(!syntax);
    xlsp_source_syntax_free(syntax);CHECK(alias_ledgers==1&&!runtime_live&&!runtime_bytes);return s;
}
static void rejected_aliases(bool full) {
    for(unsigned mode=0;mode<4;++mode){alias_reset(SIZE_MAX,SIZE_MAX);XrCompileResourceStats needed={0};
        CHECK(rejected_once(mode,limits,&needed)==XR_XIR_OK);size_t source=runtime_attempts;
        if(full){for(size_t prefix=0;prefix<source;++prefix){alias_reset(prefix,SIZE_MAX);
                CHECK(rejected_once(mode,limits,NULL)==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>prefix);}
            for(int delta=-1;delta<=1;++delta){alias_reset(SIZE_MAX,SIZE_MAX);
                CHECK(needed.peak_bytes&&needed.peak_bytes<INT64_MAX);
                XrCompileResourceLimits exact={XR_COMPILE_MEMORY_FINITE,delta<0?needed.peak_bytes-1:needed.peak_bytes+(unsigned)delta,true};
            CHECK(rejected_once(mode,exact,NULL)==(delta<0?XR_XIR_MEMORY_LIMIT:XR_XIR_OK));}}
        alias_reset(SIZE_MAX,SIZE_MAX);CHECK(rejected_once(mode,limits,NULL)==XR_XIR_OK);printf("{\"method\":2,\"mode\":%u,\"source\":%zu,\"json\":0,\"full\":%s,\"boundaries\":%u,\"ledgers\":1,\"physical_live\":0,\"physical_bytes\":0}\n",mode,source,full?"true":"false",full?3u:0u);
    }
}
int main(int argc,char **argv) {
    (void)sdk_fixture_malloc;(void)sdk_fixture_free;(void)sdk_ledger;(void)sdk_unlimited;
    CHECK(argc==1||(argc==2&&!strcmp(argv[1],"--full")));bool full=argc==2;
    for(unsigned method=0;method<2;++method)for(unsigned mode=0;mode<9;++mode)alias_resources(method,mode,full);
    rejected_aliases(full);return 0;
}
