/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_calls.c - Actual call handlers use canonical identity and own their results
 */
#include "app/lsp/xlsp_call_hierarchy.h"
#include "app/lsp/xlsp_source_bindings.h"
#include "base/xwindows_utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d:%s\n",__LINE__,#c);exit(1);}}while(0)
#define xr_compile_resources_new call_resource_new_impl
#define xr_compile_resources_release call_resource_release_impl
#include "xir_sdk_resource_test.h"
#undef xr_compile_resources_new
#undef xr_compile_resources_release
static XrCompileResourceStats call_last_stats;
static size_t call_ledgers;
static const XrCompileResourceLimits *call_axis_limits;
static bool call_axis_active,call_axis_budget;
XR_FUNC XrCompileResourceStatus xr_compile_resources_new(const XrCompileResourceLimits *limits,XrCompileResources **out) {
    CHECK(limits&&limits->allocated_bytes==UINT64_C(64)*1024*1024&&limits->live_bytes==UINT64_C(8)*1024*1024&&limits->work==UINT64_C(128000000));
    ++call_ledgers;return call_resource_new_impl(call_axis_active?call_axis_limits:limits,out);
}
XR_FUNC void xr_compile_resources_release(XrCompileResources *resources) {
    if(resources)call_last_stats=sdk_stats(resources);call_resource_release_impl(resources);
}
#include "lsp_open_json_owner.h"
static const char root_text[]="/* \xf0\x9f\x98\x80 */ import {target as chosen} from \"./lib\"\r\nfn wrapper(v:i64)->i64 {\r\n return chosen(v)+chosen(v)\r\n}\r\nconst answer=wrapper(1)\r\n";
static void destroyed(XrLspDocument *doc){doc->uri=NULL;doc->content=NULL;doc->server=NULL;}
static XrJsonValue *integer(unsigned n) {XrJsonValue *v=xjson_new_number(0);CHECK(v);v->is_integer=true;v->as.integer=n;return v;}
static XrJsonValue *prepare_params(const char *uri,unsigned line,unsigned column) {
    XrJsonValue *p=xjson_new_object(),*d=xjson_new_object(),*at=xjson_new_object();CHECK(p&&d&&at);
    xjson_object_set(d,"uri",xjson_new_string(uri));xjson_object_set(at,"line",integer(line));xjson_object_set(at,"character",integer(column));
    xjson_object_set(p,"textDocument",d);xjson_object_set(p,"position",at);return p;
}
static XrJsonValue *item_params(XrJsonValue *item) {
    size_t length=0;char *text=xjson_stringify(item,&length);CHECK(text);
    XrJsonValue *copy=xjson_parse(text,length);CHECK(copy);lsp_json_free(text);
    XrJsonValue *p=xjson_new_object();CHECK(p);xjson_object_set(p,"item",copy);return p;
}
static XrJsonValue *invoke(XrLspServer *s,XrJsonValue *params,unsigned mode) {
    XrJsonValue *result=mode==0?xlsp_handle_prepare_call_hierarchy(s,params):mode==1?xlsp_handle_call_hierarchy_incoming(s,params):xlsp_handle_call_hierarchy_outgoing(s,params);
    if(!result)fprintf(stderr,"call handler mode=%u stage=%u status=%u\n",mode,s->source_failure.stage,s->source_failure.status);
    CHECK(result&&result->type==XR_JSON_ARRAY&&s->source_failure.status==XR_XIR_OK&&!s->source_failure.stage);
    CHECK(!runtime_live&&!runtime_bytes);return result;
}
static bool named(XrJsonValue *item,const char *name){const char *actual=xjson_get_string(item,"name");return actual&&!strcmp(actual,name);}
static void range(XrJsonValue *object,const char *key,int line,int column,int end_line,int end_column) {
    XrJsonValue *r=xjson_get_object(object,key),*a=xjson_get_object(r,"start"),*b=xjson_get_object(r,"end");CHECK(a&&b);
    if(xjson_get_int(a,"line")!=line||xjson_get_int(a,"character")!=column||xjson_get_int(b,"line")!=end_line||xjson_get_int(b,"character")!=end_column)
        fprintf(stderr,"range %s got %lld:%lld..%lld:%lld expected%d:%d..%d:%d\n",key,(long long)xjson_get_int(a,"line"),(long long)xjson_get_int(a,"character"),(long long)xjson_get_int(b,"line"),(long long)xjson_get_int(b,"character"),line,column,end_line,end_column);
    CHECK(xjson_get_int(a,"line")==line&&xjson_get_int(a,"character")==column&&xjson_get_int(b,"line")==end_line&&xjson_get_int(b,"character")==end_column);
}
static XrJsonValue *find_edge(XrJsonValue *array,const char *key,const char *name) {
    XrJsonValue *found=NULL;for(int i=0;i<array->as.array.count;++i){XrJsonValue *edge=xjson_array_get(array,i);if(named(xjson_get_object(edge,key),name)){CHECK(!found);found=edge;}}
    CHECK(found);return found;
}
static void normal(char **argv) {
    XrLspServer server={0};server.doc_table=xlsp_document_store_new(destroyed);CHECK(server.doc_table);server.source_revision=41;
    XrLspDocument root={0};root.server=&server;root.uri=argv[1];root.content=(char *)root_text;root.length=sizeof(root_text)-1;root.version=7;root.ast=(AstNode *)(uintptr_t)1;
    XlspDocumentInsertion insertion={0};CHECK(xlsp_document_store_prepare(server.doc_table,root.uri,&root,&insertion)&&xlsp_document_store_commit(&insertion));
    XrJsonValue *p=prepare_params(argv[1],2,8),*prepared=invoke(&server,p,0);CHECK(prepared->as.array.count==1);
    XrJsonValue *target=xjson_array_get(prepared,0);CHECK(named(target,"target")&&!strcmp(xjson_get_string(target,"uri"),argv[2]));
    range(target,"range",0,10,2,1);range(target,"selectionRange",0,10,0,16);xjson_free(p);
    XrJsonValue *stale_params=item_params(target);
    p=item_params(target);XrJsonValue *incoming=invoke(&server,p,1);CHECK(incoming->as.array.count==2);
    XrJsonValue *wrapper=find_edge(incoming,"from","wrapper"),*external=find_edge(incoming,"from","external");
    CHECK(xjson_get(wrapper,"fromRanges")->as.array.count==2&&xjson_get(external,"fromRanges")->as.array.count==1);
    range(xjson_get_object(wrapper,"from"),"range",1,3,3,1);range(xjson_get_object(wrapper,"from"),"selectionRange",1,3,1,10);xjson_free(p);
    xjson_free(prepared);prepared=NULL;
    p=item_params(xjson_get_object(wrapper,"from"));XrJsonValue *outgoing=invoke(&server,p,2);CHECK(outgoing->as.array.count==1);
    XrJsonValue *edge=find_edge(outgoing,"to","target");CHECK(xjson_get(edge,"fromRanges")->as.array.count==2);
    range(xjson_get_object(edge,"to"),"range",0,10,2,1);xjson_free(p);
    p=item_params(xjson_get_object(wrapper,"from"));xjson_free(incoming);incoming=NULL;
    XrJsonValue *parents=invoke(&server,p,1);CHECK(parents->as.array.count==1);
    XrJsonValue *module=xjson_get_object(xjson_array_get(parents,0),"from");CHECK(named(module,"module initialization"));xjson_free(p);
    p=item_params(module);xjson_free(parents);parents=NULL;
    XrJsonValue *module_calls=invoke(&server,p,2);CHECK(module_calls->as.array.count==1&&named(xjson_get_object(xjson_array_get(module_calls,0),"to"),"wrapper"));xjson_free(p);xjson_free(module_calls);module_calls=NULL;
    ++server.source_revision;XrJsonValue *stale=xlsp_handle_call_hierarchy_incoming(&server,stale_params);CHECK(!stale&&server.source_failure.status==XR_XIR_UNRESOLVED&&server.source_failure.stage==24);xjson_free(stale_params);
    CHECK(root.ast==(AstNode *)(uintptr_t)1);xlsp_document_store_free(server.doc_table);CHECK(!runtime_live&&!runtime_bytes);
    CHECK(named(xjson_get_object(find_edge(outgoing,"to","target"),"to"),"target"));
    xjson_free(prepared);xjson_free(incoming);xjson_free(outgoing);xjson_free(parents);xjson_free(module_calls);CHECK(!lsp_json_live&&!lsp_json_bytes);
    puts("Actual call handlers: alias/closed incoming, grouped repeated sites, definition bounds, module initializer, stale revision, no AST read, producer death physical0/0");
}
#include "call_cases.h"
#include "call_resources.h"
#include "call_protocol.h"
static int run(int argc,char **argv){CHECK(argc==5||argc==8||argc==9||argc==10);(void)sdk_fixture_malloc;(void)sdk_fixture_free;(void)sdk_ledger;(void)sdk_unlimited;(void)sdk_stats;
    if(argc==9){CHECK(!strcmp(argv[4],"--axes"));return resource_axes(argv);}
    if(argc==10){CHECK(!strcmp(argv[4],"--chunk"));return resource_chunk(argc,argv);}
    if(argc==8){CHECK(!strcmp(argv[4],"--resource"));return resource_once(argv,(unsigned)strtoul(argv[5],NULL,10),(size_t)strtoull(argv[6],NULL,10),(size_t)strtoull(argv[7],NULL,10),true);}
    unsigned mode=(unsigned)strtoul(argv[4],NULL,10);CHECK(mode<9);
    switch(mode){case 0:normal(argv);break;case 1:closure_case(argv);break;case 2:recursion_case(argv);break;case 3:method_case(argv);break;case 4:utf16_case(argv);break;case 5:indirect_case(argv);break;case 6:local_closure_case(argv);break;case 7:protocol_case(argv);break;case 8:binding_roots_case(argv);break;}
    return 0;}
int wmain(int argc,wchar_t **wide){XrWinPathStatus s;char **argv=xr_win_utf16_arguments(argc,wide,&s);CHECK(argv);int result=run(argc,argv);xr_win_utf8_arguments_free(argc,argv);return result;}
