/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_source_on_type.h"
#include "app/lsp/xlsp_on_type.h"
#include "app/lsp/xlsp_server.h"
#include "app/lsp/xlsp_document_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"
#include "lsp_open_json_owner.h"
typedef struct Example {const char *text,*indent;XrLspPosition position;char trigger;bool spaces;unsigned replaced;} Example;
static const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
static const Example examples[]={
    {"fn run(){\n","    ",{1,0},'\n',true,0},
    {"fn run(){\nvar x=\"{\"\n  }","",{2,3},'}',true,2},
    {"fn run(){\nif(true){\n       }","    ",{2,8},'}',true,7},
    {"fn run(){ // {\n","    ",{1,0},'\n',true,0},
    {"\tfn run(){\n  ","\t\t",{1,2},'\n',false,2},
    {"fn run(){var r=/[{}]/\n  }","",{1,3},'}',true,2},
    {"fn run(){/*\n",NULL,{1,0},'\n',true,0},
    {"var x=\"\"\"\n",NULL,{1,0},'\n',true,0},
    {"fn run(){\r  }","",{1,3},'}',true,2},
    {"/* \xf0\x9f\x98\x80 } */fn run(){\r\n","    ",{1,0},'\n',true,0},
    {"var x=1;",NULL,{0,8},';',true,0},
    {"",NULL,{0,0},'\n',true,0},
    {"fn run(){\n}",NULL,{1,1},'}',true,0},
    {" \tfn run(){\n","\t\t",{1,0},'\n',false,0},
    {"fn run(){/*\n  }",NULL,{1,3},'}',true,0},
    {"var x=\"unterminated\n",NULL,{1,0},'\n',true,0},
    {"  }",NULL,{0,3},'}',true,0},
    {"",NULL,{1,3},'}',true,2},
    {"fn run(){}",NULL,{0,10},'}',true,0},
    {"fn run(){\n  \n","  ",{2,0},'\n',true,0},
    {"fn run(){\n??","    ",{1,0},'\n',true,0},
    {"var m=#{\n","    ",{1,0},'\n',true,0},
    {"fn run(){\nvar x=1",NULL,{1,3},'\n',true,0}
};
static void facts(XrJsonValue *result,unsigned mode) {
    const Example *e=&examples[mode];CHECK(result&&result->type==XR_JSON_ARRAY&&result->as.array.count==((e->indent||mode==17)?1:0));
    if(!result->as.array.count)return;
    XrJsonValue *edit=xjson_array_get(result,0),*range=xjson_get_object(edit,"range"),*start=xjson_get_object(range,"start"),*end=xjson_get_object(range,"end"),*text=xjson_get(edit,"newText");
    CHECK(start&&end&&text&&text->type==XR_JSON_STRING&&xjson_get_int(start,"line")==e->position.line&&xjson_get_int(start,"character")==0&&
        xjson_get_int(end,"line")==e->position.line&&xjson_get_int(end,"character")==e->replaced);
    if(mode==17){CHECK(text->string_len==276);for(size_t i=0;i<text->string_len;++i)CHECK(text->as.string[i]==' ');}
    else CHECK(text->string_len==strlen(e->indent)&&!memcmp(text->as.string,e->indent,text->string_len));
}
static XrXirStatus once(unsigned mode,XrCompileResourceLimits caps,XrCompileResourceStats *stats,size_t fault,bool *injected) {
    XrCompileResources *r=NULL;XrCompileResourceStatus made=xr_compile_resources_new(&caps,&r);if(made!=XR_COMPILE_RESOURCE_OK)return made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    char text[512],uri[]="file:///on-type.xr";const Example *e=&examples[mode];strcpy(text,e->text);
    if(mode==17){memset(text,'{',70);memcpy(text+70,"\n  }",5);}
    XlspSourceDocument document={uri,text,sizeof(uri)-1,strlen(text),7};XlspSourceOnTypeInput input={e->position,4,e->spaces,e->trigger};XrJsonValue *result=NULL;
    lsp_json_calls=0;lsp_json_fail=fault;lsp_json_injected=false;
    XrXirStatus status=xlsp_source_on_type_json(r,&document,&input,&result);
    if(stats){*stats=sdk_stats(r);if(status!=XR_XIR_OK)fprintf(stderr,"On-type mode=%u status=%u\n",mode,(unsigned)status);}
    if(injected)*injected=lsp_json_injected;lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
    if(status==XR_XIR_OK){facts(result,mode);XrJsonValue *occupied=result;size_t before=runtime_attempts,json_before=lsp_json_calls;
        CHECK(xlsp_source_on_type_json(r,&document,&input,&occupied)==XR_XIR_BAD_STRUCTURE&&occupied==result&&runtime_attempts==before&&lsp_json_calls==json_before);}
    else CHECK(!result);
    memset(text,'?',sizeof(text));memset(uri,'?',sizeof(uri));xr_compile_resources_release(r);CHECK(!runtime_live&&!runtime_bytes);
    if(status==XR_XIR_OK)facts(result,mode);xjson_free(result);CHECK(!lsp_json_live&&!lsp_json_bytes);return status;
}
static void invalid(void) {
    XrCompileResources *r=sdk_ledger(&limits);XrJsonValue *result=NULL;XlspSourceDocument d={"file:///x.xr","\xf0\x9f\x98\x80",12,4,1};XlspSourceOnTypeInput input={{0,1},4,true,';'};
    CHECK(xlsp_source_on_type_json(r,&d,&input,&result)==XR_XIR_BAD_STRUCTURE&&!result);
    input.position=(XrLspPosition){1,0};CHECK(xlsp_source_on_type_json(r,&d,&input,&result)==XR_XIR_BAD_STRUCTURE&&!result);
    input.position=(XrLspPosition){0,3};CHECK(xlsp_source_on_type_json(r,&d,&input,&result)==XR_XIR_BAD_STRUCTURE&&!result);
    input.position=(XrLspPosition){0,0};d.text="\x80";d.length=1;CHECK(xlsp_source_on_type_json(r,&d,&input,&result)==XR_XIR_BAD_STRUCTURE&&!result);
    d.text="\0";CHECK(xlsp_source_on_type_json(r,&d,&input,&result)==XR_XIR_BAD_STRUCTURE&&!result);
    d.text="";d.length=0;d.uri="file:\0bad";d.uri_length=9;CHECK(xlsp_source_on_type_json(r,&d,&input,&result)==XR_XIR_BAD_STRUCTURE&&!result);
    d.uri="file:///x.xr";d.uri_length=12;input.tab_size=0;size_t before=runtime_attempts;CHECK(xlsp_source_on_type_json(r,&d,&input,&result)==XR_XIR_BAD_STRUCTURE&&!result&&runtime_attempts==before);
    input.tab_size=4;input.trigger='x';CHECK(xlsp_source_on_type_json(r,&d,&input,&result)==XR_XIR_BAD_STRUCTURE&&!result&&runtime_attempts==before);
    xr_compile_resources_release(r);CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes);
}
static unsigned destroyed;
static void destroy_document(XrLspDocument *d){CHECK(d);d->content=NULL;d->uri=NULL;d->server=NULL;++destroyed;}
static XrXirStatus adapter(size_t fault,bool *injected) {
    XrLspServer server={0};server.doc_table=xlsp_document_store_new(destroy_document);CHECK(server.doc_table);destroyed=0;
    char text[]="fn run(){\n",uri[]="file:///on-type.xr";XrLspDocument doc={0};doc.server=&server;doc.uri=uri;doc.content=text;doc.length=sizeof(text)-1;doc.version=7;
    XlspDocumentInsertion prepared={0};CHECK(xlsp_document_store_prepare(server.doc_table,uri,&doc,&prepared)&&xlsp_document_store_commit(&prepared));
    const char *wire="{\"textDocument\":{\"uri\":\"file:///on-type.xr\"},\"position\":{\"line\":1,\"character\":0},\"ch\":\"\\n\",\"options\":{\"tabSize\":4,\"insertSpaces\":true}}";
    XrJsonValue *params=xjson_parse(wire,strlen(wire));CHECK(params);lsp_json_calls=0;lsp_json_fail=fault;lsp_json_injected=false;
    XrJsonValue *result=xlsp_on_type_handle(&server,params);XrXirStatus status=server.source_failure.stage?(XrXirStatus)server.source_failure.status:XR_XIR_OK;
    if(injected)*injected=lsp_json_injected;lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
    xjson_free(params);xlsp_document_store_free(server.doc_table);memset(text,'?',sizeof(text));memset(uri,'?',sizeof(uri));CHECK(destroyed==1&&!runtime_live&&!runtime_bytes);
    if(status==XR_XIR_OK)facts(result,0);else CHECK(!result);xjson_free(result);CHECK(!lsp_json_live&&!lsp_json_bytes);return status;
}
static void adapter_invalid(void) {
    const char *wire="{\"textDocument\":{\"uri\":\"file:///on-type.xr\"},\"position\":{\"line\":1,\"character\":0},\"ch\":\"\\n\",\"options\":{\"tabSize\":4,\"insertSpaces\":true}}";
    const struct {const char *object,*key,*value;} cases[]={
        {"position","line","-1"},{"position","line","2147483648"},{"position","line","1.5"},
        {"position","character","-1"},{"position","character","2147483648"},{"position","character","false"},
        {"options","tabSize","0"},{"options","tabSize","2147483648"},{"options","tabSize","4.5"},
        {"options","insertSpaces","\"true\""},{NULL,"ch","\"}}\""},{NULL,"ch","\"x\""},
        {"textDocument","uri","123"},{"textDocument","uri","\"file:\\u0000bad\""},{NULL,"position","[]"}
    };
    XrLspServer server={0};server.doc_table=xlsp_document_store_new(destroy_document);CHECK(server.doc_table);
    XrLspDocument doc={0};doc.server=&server;doc.uri="file:///on-type.xr";doc.content="fn run(){\n";doc.length=strlen(doc.content);doc.version=7;
    XlspDocumentInsertion prepared={0};CHECK(xlsp_document_store_prepare(server.doc_table,doc.uri,&doc,&prepared)&&xlsp_document_store_commit(&prepared));destroyed=0;
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i){XrJsonValue *params=xjson_parse(wire,strlen(wire)),*replacement=xjson_parse(cases[i].value,strlen(cases[i].value));CHECK(params&&replacement);
        XrJsonValue *object=cases[i].object?xjson_get_object(params,cases[i].object):params;CHECK(object);xjson_object_set(object,cases[i].key,replacement);CHECK(xjson_get(object,cases[i].key)==replacement);
        size_t before=runtime_attempts;CHECK(!xlsp_on_type_handle(&server,params)&&server.source_failure.stage==16&&server.source_failure.status==XR_XIR_BAD_STRUCTURE&&runtime_attempts==before);xjson_free(params);CHECK(!lsp_json_live&&!lsp_json_bytes);}
    xlsp_document_store_free(server.doc_table);CHECK(destroyed==1&&!runtime_live&&!runtime_bytes);
}
int main(int argc,char **argv) {
    (void)sdk_fixture_malloc;(void)sdk_fixture_free;(void)sdk_unlimited;
    CHECK(argc==1||(argc==2&&!strcmp(argv[1],"--normal")));bool full=argc==1;
    for(unsigned mode=0;mode<sizeof(examples)/sizeof(*examples);++mode) {
        XrCompileResourceStats stats={0};runtime_attempts=0;runtime_fail_at=SIZE_MAX;CHECK(once(mode,limits,&stats,SIZE_MAX,NULL)==XR_XIR_OK);size_t sites=runtime_attempts,json_sites=lsp_json_calls;
        if(full){for(size_t i=0;i<sites;++i){runtime_attempts=0;runtime_fail_at=i;CHECK(once(mode,limits,NULL,SIZE_MAX,NULL)==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>i);}runtime_fail_at=SIZE_MAX;
            for(size_t i=0;i<json_sites;++i){bool injected=false;CHECK(once(mode,limits,NULL,i,&injected)==XR_XIR_OUT_OF_MEMORY&&injected);}
            for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){XrCompileResourceLimits exact={stats.allocated_bytes,stats.peak_bytes,stats.work};uint64_t *bound=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;CHECK(*bound&&*bound<INT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);CHECK(once(mode,exact,NULL,SIZE_MAX,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));}}
        printf("Source on-type mode=%u SourceFI=%zu JSONFI=%zu full=%u prefix lexer/strictUTF16/full indent physical0/0\n",mode,sites,json_sites,full?1u:0u);
    }
    invalid();adapter_invalid();runtime_attempts=0;CHECK(adapter(SIZE_MAX,NULL)==XR_XIR_OK);size_t sites=runtime_attempts,json_sites=lsp_json_calls;
    if(full){for(size_t i=0;i<sites;++i){runtime_attempts=0;runtime_fail_at=i;CHECK(adapter(SIZE_MAX,NULL)==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>i);}runtime_fail_at=SIZE_MAX;
        for(size_t i=0;i<json_sites;++i){bool injected=false;CHECK(adapter(i,&injected)==XR_XIR_OUT_OF_MEMORY&&injected);}}
    printf("Actual on-type adapter SourceFI=%zu JSONFI=%zu full=%u document death physical0/0\n",sites,json_sites,full?1u:0u);return 0;
}
