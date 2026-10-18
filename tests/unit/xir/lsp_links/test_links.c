/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_source_links.h"
#include "app/lsp/xlsp_links.h"
#include "base/xwindows_utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"
#include "lsp_open_json_owner.h"

typedef struct Inputs {const char *root,*library,*disk,*long_uri,*long_specifier,*math,*invalid_project;} Inputs;
static const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
static const char *const sources[]={"import \"./lib\"\nimport \"./disk\"\n",
    "// import \"./ghost\"\nfn text()->string{return \"import ./fake\"}\nfn pattern(){var pattern=/import\\{fake\\}/}\nimport { value as selected } from \"./lib\"\n",
    "import \"\"\"\n    ./lib\n    \"\"\"\n",
    "/* \xf0\x9f\x98\x80 */ import \"./lib\";\r\nimport \"./disk\";\rimport \"./lib\"\n",
    "import math\n",NULL,"fn value()->i64{return 1}","import \"./lib\" as u\nimport \"./lib\" as v\n",
    "import \"./disk\"\nexport * from \"./disk\"",
    "export { value as selected } from \"./lib\"\n",
    "export * from \"\"\"\n    ./lib\n    \"\"\"\n",
    "/* \xf0\x9f\x98\x80 */ export * from \"./lib\";\r\nexport { value as x } from \"./lib\";\rexport * from math\n"};
static void link_fact(XrJsonValue *array,int index,const char *target,int line,int column,int end_line,int end_column) {
    XrJsonValue *link=xjson_array_get(array,index),*range=xjson_get_object(link,"range");
    XrJsonValue *start=xjson_get_object(range,"start"),*end=xjson_get_object(range,"end"),*uri=xjson_get(link,"target");
    if(xjson_get_int(start,"line")!=line||xjson_get_int(start,"character")!=column||xjson_get_int(end,"line")!=end_line||xjson_get_int(end,"character")!=end_column)
        fprintf(stderr,"link range got %lld:%lld..%lld:%lld expected %d:%d..%d:%d\n",(long long)xjson_get_int(start,"line"),(long long)xjson_get_int(start,"character"),(long long)xjson_get_int(end,"line"),(long long)xjson_get_int(end,"character"),line,column,end_line,end_column);
    CHECK(start&&end&&uri&&uri->type==XR_JSON_STRING&&uri->string_len==strlen(target)&&!memcmp(uri->as.string,target,uri->string_len));
    CHECK(xjson_get_int(start,"line")==line&&xjson_get_int(start,"character")==column&&
        xjson_get_int(end,"line")==end_line&&xjson_get_int(end,"character")==end_column);
}
static void facts(XrJsonValue *result,const Inputs *in,unsigned mode) {
    CHECK(result&&result->type==XR_JSON_ARRAY);int count=mode==0||mode==7||mode==8?2:mode==3||mode==11?3:mode==6?0:1;CHECK(result->as.array.count==count);
    if(mode==0){link_fact(result,0,in->library,0,7,0,14);link_fact(result,1,in->disk,1,7,1,15);}
    else if(mode==1)link_fact(result,0,in->library,3,34,3,41);
    else if(mode==2)link_fact(result,0,in->library,0,7,2,7);
    else if(mode==3){link_fact(result,0,in->library,0,16,0,23);link_fact(result,1,in->disk,1,7,1,15);link_fact(result,2,in->library,2,7,2,14);}
    else if(mode==4)link_fact(result,0,in->math,0,7,0,11);
    else if(mode==5){CHECK(strlen(in->long_uri)>512);link_fact(result,0,in->long_uri,0,7,0,9+(int)strlen(in->long_specifier));}
    else if(mode==7){link_fact(result,0,in->library,0,7,0,14);link_fact(result,1,in->library,1,7,1,14);}
    else if(mode==8){link_fact(result,0,in->disk,0,7,0,15);link_fact(result,1,in->disk,1,14,1,22);}
    else if(mode==9)link_fact(result,0,in->library,0,34,0,41);
    else if(mode==10)link_fact(result,0,in->library,0,14,2,7);
    else if(mode==11){link_fact(result,0,in->library,0,23,0,30);link_fact(result,1,in->library,1,27,1,34);link_fact(result,2,in->math,2,14,2,18);}
}
static XrXirStatus once(const Inputs *in,unsigned mode,XrCompileResourceLimits caps,XrCompileResourceStats *stats,size_t json_fault,bool *injected) {
    XrCompileResources *r=NULL;XlspSourceLinks *links=NULL;XrJsonValue *result=NULL;
    XrCompileResourceStatus made=xr_compile_resources_new(&caps,&r);
    if(made!=XR_COMPILE_RESOURCE_OK)return made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    char text[2048],root_uri[2048],lib_uri[2048],child[]="fn unfinished(";
    int written=mode==5?snprintf(text,sizeof(text),"import \"%s\"\n",in->long_specifier):snprintf(text,sizeof(text),"%s",sources[mode]);
    CHECK(written>=0&&(size_t)written<sizeof(text));CHECK(strlen(in->root)<sizeof(root_uri));strcpy(root_uri,in->root);
    const char *target=mode==5?in->long_uri:in->library;CHECK(strlen(target)<sizeof(lib_uri));strcpy(lib_uri,target);
    XlspSourceDocument documents[]={{lib_uri,child,strlen(lib_uri),sizeof(child)-1,3},{root_uri,text,strlen(root_uri),(size_t)written,7}};
    XrXirCompileContext context={r,xr_xir_compile_default_limits()};
    lsp_json_calls=0;lsp_json_fail=json_fault;lsp_json_injected=false;
    XrXirStatus status=xlsp_source_links_build(&context,documents,2,1,&links);bool released=false;
    memset(text,'?',sizeof(text));memset(root_uri,'?',sizeof(root_uri));memset(lib_uri,'?',sizeof(lib_uri));memset(child,'?',sizeof(child));memset(documents,0,sizeof(documents));
    if(status==XR_XIR_OK) {
        XlspSourceLinks *occupied=links;size_t before=runtime_attempts;
        CHECK(xlsp_source_links_build(&context,documents,2,1,&occupied)==XR_XIR_BAD_STRUCTURE&&occupied==links&&before==runtime_attempts);
        xr_compile_resources_release(r);released=true;
        status=xlsp_source_links_json(links,&result);
        if(status==XR_XIR_OK){facts(result,in,mode);XrJsonValue *occupied_json=result;size_t json_before=lsp_json_calls;
            CHECK(xlsp_source_links_json(links,&occupied_json)==XR_XIR_BAD_STRUCTURE&&occupied_json==result&&json_before==lsp_json_calls);}
    } else CHECK(!links);
    if(injected)*injected=lsp_json_injected;lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
    if(status!=XR_XIR_OK)CHECK(!result);
    if(stats){*stats=sdk_stats(r);if(status!=XR_XIR_OK)fprintf(stderr,"Link diagnostic mode=%u status=%u\n",mode,(unsigned)status);}
    if(!released)xr_compile_resources_release(r);xlsp_source_links_free(links);CHECK(!runtime_live&&!runtime_bytes);
    if(status==XR_XIR_OK)facts(result,in,mode);xjson_free(result);CHECK(!lsp_json_live&&!lsp_json_bytes);return status;
}
static void rejected(const Inputs *in) {
    XrCompileResources *r=sdk_ledger(&limits);XlspSourceLinks *links=NULL;XrXirCompileContext context={r,xr_xir_compile_default_limits()};
    const char *cases[]={"import \"./disk\"\nimport \"./missing\"\n","import \"../outside\"","import {"};
    XrXirStatus expected[]={XR_XIR_UNRESOLVED,XR_XIR_BAD_STRUCTURE,XR_XIR_UNRESOLVED};
    XlspSourceDocument document={in->root,NULL,strlen(in->root),0,1};
    for(unsigned i=0;i<3;++i){document.text=cases[i];document.length=strlen(cases[i]);CHECK(xlsp_source_links_build(&context,&document,1,0,&links)==expected[i]&&!links);}
    document.uri=in->invalid_project;document.uri_length=strlen(document.uri);document.text="";document.length=0;
    CHECK(xlsp_source_links_build(&context,&document,1,0,&links)==XR_XIR_BAD_STRUCTURE&&!links);
    document.uri=in->root;document.uri_length=strlen(in->root);document.text="import \"./disk\"";document.length=strlen(document.text);
    XlspSourceDocument duplicates[]={document,document};duplicates[1].text="different";duplicates[1].length=9;
    CHECK(xlsp_source_links_build(&context,duplicates,2,0,&links)==XR_XIR_BAD_STRUCTURE&&!links);
    xr_compile_resources_release(r);CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes);
}
static unsigned adapter_destroyed;
static void adapter_destroy(XrLspDocument *document) {
    CHECK(document);document->server=NULL;document->uri=NULL;document->content=NULL;document->length=0;++adapter_destroyed;
}
static XrXirStatus adapter_once(const Inputs *in,unsigned mode,size_t json_fault,bool *injected) {
    adapter_destroyed=0;XrLspServer server={0};server.doc_table=xlsp_document_store_new(adapter_destroy);CHECK(server.doc_table);
    XrLspDocument root={0},library={0};root.server=&server;root.uri=(char *)in->root;root.content=(char *)sources[mode];root.length=strlen(sources[mode]);root.version=7;
    library.server=&server;library.uri=(char *)in->library;library.content="fn unfinished(";library.length=strlen(library.content);library.version=3;
    XlspDocumentInsertion insertion={0};CHECK(xlsp_document_store_prepare(server.doc_table,library.uri,&library,&insertion)&&xlsp_document_store_commit(&insertion));
    insertion=(XlspDocumentInsertion){0};CHECK(xlsp_document_store_prepare(server.doc_table,root.uri,&root,&insertion)&&xlsp_document_store_commit(&insertion));
    XrJsonValue *params=xjson_new_object(),*doc=xjson_new_object();CHECK(params&&doc);xjson_object_set(doc,"uri",xjson_new_string(in->root));xjson_object_set(params,"textDocument",doc);
    lsp_json_calls=0;lsp_json_fail=json_fault;lsp_json_injected=false;XrJsonValue *result=xlsp_links_handle(&server,params);
    if(injected)*injected=lsp_json_injected;lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
    XrXirStatus status=(XrXirStatus)server.source_failure.status;
    if(status==XR_XIR_OK){CHECK(!server.source_failure.stage);facts(result,in,mode);}else CHECK(!result&&server.source_failure.stage==15);
    xjson_free(params);xlsp_document_store_free(server.doc_table);CHECK(adapter_destroyed==2&&!runtime_live&&!runtime_bytes);
    if(status==XR_XIR_OK)facts(result,in,mode);xjson_free(result);CHECK(!lsp_json_live&&!lsp_json_bytes);return status;
}
static void adapter_invalid(void) {
    const char *const invalid[]={"null","{}","{\"textDocument\":{\"uri\":123}}","{\"textDocument\":{\"uri\":\"file:\\u0000bad\"}}"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        XrLspServer server={0};XrJsonValue *params=xjson_parse(invalid[i],strlen(invalid[i]));CHECK(params);size_t before=runtime_attempts;
        CHECK(!xlsp_links_handle(&server,params)&&server.source_failure.stage==15&&server.source_failure.status==XR_XIR_BAD_STRUCTURE&&before==runtime_attempts);
        xjson_free(params);CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes);
    }
}
static int run(int argc,char **argv) {
    (void)sdk_fixture_malloc;(void)sdk_fixture_free;(void)sdk_unlimited;
    CHECK(argc==8||argc==9);bool full=argc==8;CHECK(full||!strcmp(argv[8],"--normal"));Inputs in={argv[1],argv[2],argv[3],argv[4],argv[5],argv[6],argv[7]};
    for(unsigned mode=0;mode<sizeof(sources)/sizeof(*sources);++mode) {
        XrCompileResourceStats stats={0};runtime_attempts=0;runtime_fail_at=SIZE_MAX;CHECK(once(&in,mode,limits,&stats,SIZE_MAX,NULL)==XR_XIR_OK);
        size_t sites=runtime_attempts,json_sites=lsp_json_calls;CHECK(sites&&json_sites);
        if(full) {
            for(size_t i=0;i<sites;++i){runtime_attempts=0;runtime_fail_at=i;CHECK(once(&in,mode,limits,NULL,SIZE_MAX,NULL)==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>i);}
            runtime_fail_at=SIZE_MAX;
            for(size_t i=0;i<json_sites;++i){bool injected=false;CHECK(once(&in,mode,limits,NULL,i,&injected)==XR_XIR_OUT_OF_MEMORY&&injected);}
            for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){XrCompileResourceLimits exact={stats.allocated_bytes,stats.peak_bytes,stats.work};
                uint64_t *bound=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;CHECK(*bound&&*bound<INT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
                CHECK(once(&in,mode,exact,NULL,SIZE_MAX,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));}
        }
        printf("Source links mode=%u SourceFI=%zu JSONFI=%zu full=%u; actual resolver/overlay/UTF16, producer death physical0/0\n",mode,sites,json_sites,full?1u:0u);
    }
    runtime_fail_at=SIZE_MAX;rejected(&in);adapter_invalid();runtime_attempts=0;CHECK(adapter_once(&in,0,SIZE_MAX,NULL)==XR_XIR_OK);size_t sites=runtime_attempts,json_sites=lsp_json_calls;
    if(full){for(size_t i=0;i<sites;++i){runtime_attempts=0;runtime_fail_at=i;CHECK(adapter_once(&in,0,SIZE_MAX,NULL)==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>i);}runtime_fail_at=SIZE_MAX;
        for(size_t i=0;i<json_sites;++i){bool injected=false;CHECK(adapter_once(&in,0,i,&injected)==XR_XIR_OUT_OF_MEMORY&&injected);}}
    printf("Source links actual handler mode=0 SourceFI=%zu JSONFI=%zu full=%u; real document table, atomic publication physical0/0\n",sites,json_sites,full?1u:0u);
    for(unsigned mode=8;mode<12;++mode){runtime_attempts=0;CHECK(adapter_once(&in,mode,SIZE_MAX,NULL)==XR_XIR_OK);sites=runtime_attempts;json_sites=lsp_json_calls;
        if(full){for(size_t i=0;i<sites;++i){runtime_attempts=0;runtime_fail_at=i;CHECK(adapter_once(&in,mode,SIZE_MAX,NULL)==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>i);}runtime_fail_at=SIZE_MAX;
            for(size_t i=0;i<json_sites;++i){bool injected=false;CHECK(adapter_once(&in,mode,i,&injected)==XR_XIR_OUT_OF_MEMORY&&injected);}}
        printf("Source links actual handler mode=%u SourceFI=%zu JSONFI=%zu full=%u; physical0/0\n",mode,sites,json_sites,full?1u:0u);}
    return 0;
}
#if XR_OS_WINDOWS
int wmain(int argc,wchar_t **wide) {XrWinPathStatus status;char **argv=xr_win_utf16_arguments(argc,wide,&status);CHECK(argv);int result=run(argc,argv);xr_win_utf8_arguments_free(argc,argv);return result;}
#else
int main(int argc,char **argv) {return run(argc,argv);}
#endif
