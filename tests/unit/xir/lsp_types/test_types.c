/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_types.c - Actual type hierarchy handlers consume Source relations and own JSON
 */
#include "app/lsp/xlsp_call_hierarchy.h"
#include "base/xwindows_utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d:%s\n",__LINE__,#c);exit(1);}}while(0)
#define xr_compile_resources_new hierarchy_resource_new_impl
#define xr_compile_resources_release hierarchy_resource_release_impl
#include "xir_sdk_resource_test.h"
#undef xr_compile_resources_new
#undef xr_compile_resources_release
static XrCompileResourceStats hierarchy_last_stats;
static size_t hierarchy_ledgers;
static const XrCompileResourceLimits *hierarchy_axis_limits;
static bool hierarchy_axis_active,hierarchy_axis_budget;
XR_FUNC XrCompileResourceStatus xr_compile_resources_new(const XrCompileResourceLimits *bounds,XrCompileResources **out) {
    CHECK(bounds&&bounds->allocated_bytes==UINT64_C(64)*1024*1024&&bounds->live_bytes==UINT64_C(8)*1024*1024&&bounds->work==UINT64_C(128000000));
    ++hierarchy_ledgers;return hierarchy_resource_new_impl(hierarchy_axis_active?hierarchy_axis_limits:bounds,out);
}
XR_FUNC void xr_compile_resources_release(XrCompileResources *r){if(r)hierarchy_last_stats=sdk_stats(r);hierarchy_resource_release_impl(r);}
#include "lsp_open_json_owner.h"
static const char root_text[]="/* \xf0\x9f\x98\x80 */ import {Base as Alias} from \"./lib\"\r\ninterface Left extends Alias {}\r\ninterface Right extends Alias {}\r\ninterface Both extends Left,Right {}\r\nstruct Meter implements Both {\r\n value:i64\r\n measure()->i64{return this.value}\r\n}\r\nfinal class C {}\r\nenum E { A, B }\r\n";
static void destroyed(XrLspDocument *doc){doc->uri=NULL;doc->content=NULL;doc->server=NULL;}
typedef struct TypeFixture {XrLspServer server;XrLspDocument root;} TypeFixture;
static void fixture(TypeFixture *f,char **argv,const char *text) {
    memset(f,0,sizeof(*f));f->server.doc_table=xlsp_document_store_new(destroyed);CHECK(f->server.doc_table);f->server.source_revision=41;
    f->root.server=&f->server;f->root.uri=argv[1];f->root.content=(char *)text;f->root.length=strlen(text);f->root.version=7;f->root.ast=(AstNode *)(uintptr_t)1;
    XlspDocumentInsertion insertion={0};CHECK(xlsp_document_store_prepare(f->server.doc_table,f->root.uri,&f->root,&insertion)&&xlsp_document_store_commit(&insertion));
}
static void done(TypeFixture *f){CHECK(f->root.ast==(AstNode *)(uintptr_t)1);xlsp_document_store_free(f->server.doc_table);CHECK(!runtime_live&&!runtime_bytes);}
static XrJsonValue *integer(unsigned n){XrJsonValue *v=xjson_new_number(0);CHECK(v);v->is_integer=true;v->as.integer=n;return v;}
static XrJsonValue *params(const char *uri,unsigned line,unsigned column) {
    XrJsonValue *p=xjson_new_object(),*doc=xjson_new_object(),*at=xjson_new_object();CHECK(p&&doc&&at);
    xjson_object_set(doc,"uri",xjson_new_string(uri));xjson_object_set(at,"line",integer(line));xjson_object_set(at,"character",integer(column));
    xjson_object_set(p,"textDocument",doc);xjson_object_set(p,"position",at);return p;
}
static XrJsonValue *item_params(XrJsonValue *item) {
    size_t length=0;char *text=xjson_stringify(item,&length);CHECK(text);XrJsonValue *copy=xjson_parse(text,length);CHECK(copy);lsp_json_free(text);
    XrJsonValue *p=xjson_new_object();CHECK(p);xjson_object_set(p,"item",copy);return p;
}
static XrJsonValue *request(TypeFixture *f,XrJsonValue *p,unsigned mode) {
    XrJsonValue *out=mode==0?xlsp_handle_prepare_type_hierarchy(&f->server,p):mode==1?
        xlsp_handle_type_hierarchy_supertypes(&f->server,p):xlsp_handle_type_hierarchy_subtypes(&f->server,p);
    if(!out)fprintf(stderr,"Actual type method%u stage%u status%u\n",mode,f->server.source_failure.stage,f->server.source_failure.status);
    CHECK(out&&out->type==XR_JSON_ARRAY&&!f->server.source_failure.stage&&!runtime_live&&!runtime_bytes);return out;
}
static XrJsonValue *prepare(TypeFixture *f,unsigned line,unsigned column) {
    XrJsonValue *p=params(f->root.uri,line,column),*out=request(f,p,0);xjson_free(p);CHECK(out->as.array.count==1);return out;
}
static XrJsonValue *related(TypeFixture *f,XrJsonValue *item,unsigned mode) {
    XrJsonValue *p=item_params(item),*out=request(f,p,mode);xjson_free(p);return out;
}
static bool named(XrJsonValue *v,const char *name){const char *s=xjson_get_string(v,"name");return s&&!strcmp(s,name);}
static XrJsonValue *find(XrJsonValue *values,const char *name){XrJsonValue *found=NULL;
    for(int i=0;i<values->as.array.count;++i)if(named(xjson_array_get(values,i),name)){CHECK(!found);found=xjson_array_get(values,i);}CHECK(found);return found;}
static void range(XrJsonValue *item,const char *field,unsigned sl,unsigned sc,unsigned el,unsigned ec){
    XrJsonValue *r=xjson_get_object(item,field),*a=xjson_get_object(r,"start"),*b=xjson_get_object(r,"end");
    CHECK(xjson_get_int(a,"line")==sl&&xjson_get_int(a,"character")==sc&&xjson_get_int(b,"line")==el&&xjson_get_int(b,"character")==ec);}
static void normal(char **argv) {
    TypeFixture f;fixture(&f,argv,root_text);XrJsonValue *prepared=prepare(&f,1,24),*base=xjson_array_get(prepared,0);
    CHECK(named(base,"Base")&&xjson_get_int(base,"kind")==LSP_SYMBOL_INTERFACE&&!strcmp(xjson_get_string(base,"uri"),argv[2]));
    range(base,"range",0,17,2,1);range(base,"selectionRange",0,17,0,21);
    XrJsonValue *children=related(&f,base,2);CHECK(children->as.array.count==3);find(children,"Left");find(children,"Right");XrJsonValue *external=find(children,"External");
    CHECK(!strcmp(xjson_get_string(external,"uri"),argv[3])&&xjson_get(xjson_get_object(external,"data"),"version")->type==XR_JSON_NULL);
    XrJsonValue *parents=related(&f,find(children,"Left"),1);CHECK(parents->as.array.count==1&&named(xjson_array_get(parents,0),"Base"));xjson_free(parents);
    XrJsonValue *empty=related(&f,base,1);CHECK(!empty->as.array.count);xjson_free(empty);xjson_free(prepared);prepared=prepare(&f,3,11);
    parents=related(&f,xjson_array_get(prepared,0),1);CHECK(parents->as.array.count==2&&named(xjson_array_get(parents,0),"Left")&&named(xjson_array_get(parents,1),"Right"));
    XrJsonValue *nominals=related(&f,xjson_array_get(prepared,0),2);CHECK(nominals->as.array.count==1);XrJsonValue *meter=xjson_array_get(nominals,0);
    CHECK(named(meter,"Meter")&&xjson_get_int(meter,"kind")==LSP_SYMBOL_STRUCT);range(meter,"range",4,7,7,1);range(meter,"selectionRange",4,7,4,12);
    XrJsonValue *implements=related(&f,meter,1);CHECK(implements->as.array.count==1&&named(xjson_array_get(implements,0),"Both"));
    XrJsonValue *fake=item_params(meter);xjson_object_set(xjson_get_object(fake,"item"),"detail",xjson_new_string("Base"));
    XrJsonValue *actual=request(&f,fake,1);CHECK(actual->as.array.count==1&&named(xjson_array_get(actual,0),"Both"));xjson_free(actual);xjson_free(fake);
    ++f.server.source_revision;fake=item_params(meter);CHECK(!xlsp_handle_type_hierarchy_supertypes(&f.server,fake)&&f.server.source_failure.status==XR_XIR_UNRESOLVED);xjson_free(fake);
    done(&f);CHECK(named(xjson_array_get(implements,0),"Both")&&!strcmp(xjson_get_string(xjson_array_get(implements,0),"uri"),argv[1]));
    xjson_free(prepared);xjson_free(children);xjson_free(parents);xjson_free(nominals);xjson_free(implements);CHECK(!lsp_json_live&&!lsp_json_bytes);
    puts("Actual type hierarchy: Source alias/cross-file/closed facts, immediate diamond/implements, real bounds, deterministic dedup, untrusted detail ignored, stale reject and owned producer death physical0/0");
}
static void roots(char **argv) {
    TypeFixture f;fixture(&f,argv,root_text);XrJsonValue *prepared=prepare(&f,8,12),*c=xjson_array_get(prepared,0);CHECK(named(c,"C")&&xjson_get_int(c,"kind")==LSP_SYMBOL_CLASS);
    range(c,"range",8,12,8,16);range(c,"selectionRange",8,12,8,13);XrJsonValue *up=related(&f,c,1),*down=related(&f,c,2);CHECK(!up->as.array.count&&!down->as.array.count);
    xjson_free(up);xjson_free(down);xjson_free(prepared);prepared=prepare(&f,9,5);CHECK(named(xjson_array_get(prepared,0),"E")&&xjson_get_int(xjson_array_get(prepared,0),"kind")==LSP_SYMBOL_ENUM);
    done(&f);xjson_free(prepared);CHECK(!lsp_json_live&&!lsp_json_bytes);puts("Actual type roots: checked root class and enum, exact body/name spans and empty proven relations");
}
#include "type_cases.h"
#include "type_resources.h"
static int run(int argc,char **argv){CHECK(argc==5||argc==8||argc==9||argc==10);(void)sdk_fixture_malloc;(void)sdk_fixture_free;(void)sdk_ledger;(void)sdk_unlimited;
    if(argc==10){CHECK(!strcmp(argv[4],"--chunk"));return resource_chunk(argc,argv);}
    if(argc==9){CHECK(!strcmp(argv[4],"--axes"));return resource_axes(argv);}
    if(argc==8){CHECK(!strcmp(argv[4],"--resource"));return resource_once(argv,(unsigned)strtoul(argv[5],NULL,10),(size_t)strtoull(argv[6],NULL,10),(size_t)strtoull(argv[7],NULL,10),true);}
    unsigned mode=(unsigned)strtoul(argv[4],NULL,10);CHECK(mode<8);
    switch(mode){case 0:normal(argv);break;case 1:roots(argv);break;case 2:unicode_case(argv);break;case 3:generic_case(argv);break;
        case 4:homonym_case(argv);break;case 5:protocol_case(argv);break;case 6:inheritance_required(argv);break;case 7:qualified_case(argv);break;}return 0;}
int wmain(int argc,wchar_t **wide){XrWinPathStatus s;char **argv=xr_win_utf16_arguments(argc,wide,&s);CHECK(argv);int result=run(argc,argv);xr_win_utf8_arguments_free(argc,argv);return result;}
