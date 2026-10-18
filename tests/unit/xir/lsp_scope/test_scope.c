/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_source_scope.h"
#include "app/lsp/xlsp_source_rename.h"
#include "app/lsp/xlsp_source_binding_lenses.h"
#include "app/lsp/xlsp_source_code_lens.h"
#include "app/lsp/xlsp_navigation.h"
#include "app/lsp/xlsp_document_store.h"
#include "base/xwindows_utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"
#include "lsp_open_json_owner.h"
typedef struct Inputs {const char *root,*uri,*missing,*other,*other_uri,*growth,*bad,*utf8,*large,*empty,*hidden;} Inputs;
static const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
static const char root_text[]="import \"./lib\" as lib\nfn run()->i64{return lib.target(1)}\n";
static void configuration(XlspConfig *config,XlspIgnorePattern *patterns) {
    *config=(XlspConfig){0};patterns[0]=(XlspIgnorePattern){"skip*",true,false};patterns[1]=(XlspIgnorePattern){"vendor",false,true};
    config->ignore_patterns=patterns;config->ignore_pattern_count=2;
}
static void facts(const XlspSourceScope *scope,const Inputs *in,unsigned mode) {
    size_t expected[]={4,5,2,20,0,4,6,5,20},known_expected[]={1,2,0,0,0,1,2,2,0};size_t count=xlsp_source_scope_count(scope);CHECK(count==expected[mode]);
    const XlspSourceDocument *documents=xlsp_source_scope_documents(scope);const bool *known=xlsp_source_scope_known_versions(scope);size_t versions=0;
    if(!count)CHECK(!documents&&!known);
    for(size_t i=0;i<count;++i){const XlspSourceDocument *d=&documents[i];CHECK(d->uri&&d->text&&d->uri_length==strlen(d->uri)&&d->length==strlen(d->text));
        CHECK(!strstr(d->uri,"skip.xr")&&!strstr(d->uri,"ignored.xr")&&!strstr(d->uri,"vendor"));
        if(known[i]){++versions;CHECK(d->version==7||d->version==INT32_MAX);}else CHECK(d->version==0);
        if(!strcmp(d->uri,in->uri))CHECK(known[i]&&!strcmp(d->text,root_text));}
    CHECK(versions==known_expected[mode]);
}
static XrXirStatus queries(const XlspSourceScope *scope) {
    const XlspSourceDocument *documents=xlsp_source_scope_documents(scope);
    for(size_t i=0;i<xlsp_source_scope_count(scope);++i){size_t index=SIZE_MAX;XrXirStatus s=xlsp_source_scope_find(scope,documents[i].uri,documents[i].uri_length,&index);if(s!=XR_XIR_OK)return s;CHECK(index==i);}
    size_t index=99;XrXirStatus s=xlsp_source_scope_find(scope,"missing",7,&index);if(s==XR_XIR_UNRESOLVED){CHECK(index==99);return XR_XIR_OK;}return s;
}
static XrXirStatus once(const Inputs *in,unsigned mode,XrCompileResourceLimits caps,XrCompileResourceStats *stats) {
    XrCompileResources *r=NULL;XrCompileResourceStatus made=xr_compile_resources_new(&caps,&r);if(made!=XR_COMPILE_RESOURCE_OK)return made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrXirCompileContext context={r,xr_xir_compile_default_limits()};XlspConfig config;XlspIgnorePattern patterns[2];configuration(&config,patterns);
    char uri[2048],text[256];CHECK(strlen(in->uri)<sizeof(uri));strcpy(uri,in->uri);strcpy(text,root_text);
    XlspSourceDocument documents[]={{uri,text,strlen(uri),strlen(text),7},{in->missing,"fn fresh()->i64{return 5}",strlen(in->missing),25,7}};
    documents[1].length=strlen(documents[1].text);
    const char *roots[20]={0};char root_paths[20][2048];size_t root_count=0,count=1;
    if(mode==1)count=2;
    if(mode==2||mode==3||mode==4){count=0;roots[0]=mode==2?in->other:mode==3?in->growth:in->empty;root_count=1;}
    if(mode==5){roots[0]=in->root;int n=snprintf(root_paths[0],sizeof(root_paths[0]),"%s/sub",in->root);CHECK(n>0&&(size_t)n<sizeof(root_paths[0]));roots[1]=root_paths[0];root_count=2;}
    if(mode==6){documents[1]=(XlspSourceDocument){in->other_uri,"fn opened()->i64{return 6}",strlen(in->other_uri),26,INT32_MAX};documents[1].length=strlen(documents[1].text);count=2;}
    if(mode==7){documents[1]=(XlspSourceDocument){in->hidden,"fn hidden()->i64{return 7}",strlen(in->hidden),26,7};documents[1].length=strlen(documents[1].text);count=2;}
    if(mode==8){count=0;root_count=20;for(size_t i=0;i<20;++i){int n=snprintf(root_paths[i],sizeof(root_paths[i]),"%s/part%02zu",in->growth,i);CHECK(n>0&&(size_t)n<sizeof(root_paths[i]));roots[i]=root_paths[i];}}
    XlspSourceScopeOptions options={&context,documents,count,roots,root_count,4096,&config};XlspSourceScope *scope=NULL;
    XrXirStatus status=xlsp_source_scope_capture(&options,&scope);
    if(status!=XR_XIR_OK)CHECK(!scope);
    if(status==XR_XIR_OK)status=queries(scope);
    if(status==XR_XIR_OK){facts(scope,in,mode);XlspSourceScope *occupied=scope;size_t before=runtime_attempts;CHECK(xlsp_source_scope_capture(&options,&occupied)==XR_XIR_BAD_STRUCTURE&&occupied==scope&&runtime_attempts==before);}
    if(stats){*stats=sdk_stats(r);if(status!=XR_XIR_OK)fprintf(stderr,"Scope mode=%u status=%u\n",mode,(unsigned)status);}
    memset(uri,'?',sizeof(uri));memset(text,'?',sizeof(text));memset(documents,0,sizeof(documents));memset(patterns,0,sizeof(patterns));memset(&config,0,sizeof(config));memset(root_paths,'?',sizeof(root_paths));
    xr_compile_resources_release(r);if(status==XR_XIR_OK){CHECK(runtime_live&&runtime_bytes);facts(scope,in,mode);}xlsp_source_scope_free(scope);CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes);return status;
}
static bool reference_glob(const char *p,const char *s) {
    if(!*p)return !*s;if(*p=='*')return reference_glob(p+1,s)||(*s&&reference_glob(p,s+1));
    return *s&&(*p=='?'||*p==*s)&&reference_glob(p+1,s+1);
}
static void ignores(void) {
    XrCompileResources *r=sdk_ledger(&limits);XlspConfig config={0};XlspIgnorePattern item={0};config.ignore_patterns=&item;config.ignore_pattern_count=1;
    size_t combinations=0;
    for(unsigned p=0;p<341;++p){char pattern[5]={0};unsigned x=p;size_t length=0;while(x){--x;pattern[length++]="ab?*"[x%4];x/=4;}item.pattern=pattern;item.is_glob=true;
        for(unsigned n=0;n<121;++n){char name[5]={0};unsigned y=n;size_t used=0;while(y){--y;name[used++]="ab*"[y%3];y/=3;}bool expected=reference_glob(pattern,name),actual=!expected;
            CHECK(xlsp_source_ignore(r,&config,name,false,&actual)==XR_XIR_OK&&actual==expected);CHECK(xlsp_config_should_ignore(&config,name,false)==expected);++combinations;}}
    item=(XlspIgnorePattern){"*",false,false};bool ignored=false;CHECK(xlsp_source_ignore(r,&config,"abc",false,&ignored)==XR_XIR_OK&&!ignored);CHECK(xlsp_source_ignore(r,&config,"*",false,&ignored)==XR_XIR_OK&&ignored);
    item=(XlspIgnorePattern){"vendor",false,true};CHECK(xlsp_source_ignore(r,&config,"vendor",false,&ignored)==XR_XIR_OK&&!ignored);CHECK(xlsp_source_ignore(r,&config,"vendor",true,&ignored)==XR_XIR_OK&&ignored);
    CHECK(xlsp_source_ignore(r,&config,".hidden",false,&ignored)==XR_XIR_OK&&ignored);CHECK(!xlsp_config_should_ignore(NULL,".hidden",true));
    XrCompileResourceStats stats=sdk_stats(r);CHECK(stats.work>0);xr_compile_resources_release(r);CHECK(!runtime_live&&!runtime_bytes);
    XrCompileResourceLimits small=limits;small.work=1;r=sdk_ledger(&small);ignored=true;CHECK(xlsp_source_ignore(r,&config,"vendor",true,&ignored)==XR_XIR_BUDGET&&ignored);xr_compile_resources_release(r);CHECK(!runtime_live&&!runtime_bytes);
    printf("Shared ignore policy %zu independent glob controls, literal/dir/hidden and exact resource rejection PASS\n",combinations);
}
static void rejected(const Inputs *in) {
    const char *roots[]={in->bad,in->utf8,in->large,".",in->uri};
    const XrXirStatus expected[]={XR_XIR_BAD_STRUCTURE,XR_XIR_BAD_STRUCTURE,XR_XIR_BUDGET,XR_XIR_BAD_STRUCTURE,XR_XIR_BAD_STRUCTURE};
    for(unsigned i=0;i<5;++i){XrCompileResources *r=sdk_ledger(&limits);XrXirCompileContext context={r,xr_xir_compile_default_limits()};XlspConfig config={0};XlspSourceScope *scope=NULL;
        XlspSourceScopeOptions options={&context,NULL,0,&roots[i],1,4096,&config};XrXirStatus status=xlsp_source_scope_capture(&options,&scope);if(status!=expected[i])fprintf(stderr,"Scope reject=%u actual=%u expected=%u\n",i,(unsigned)status,(unsigned)expected[i]);CHECK(status==expected[i]&&!scope);xr_compile_resources_release(r);CHECK(!runtime_live&&!runtime_bytes);}
    XrCompileResources *r=sdk_ledger(&limits);XrXirCompileContext context={r,xr_xir_compile_default_limits()};XlspConfig config;XlspIgnorePattern patterns[2];configuration(&config,patterns);
    XlspSourceDocument documents[]={{in->uri,root_text,strlen(in->uri),sizeof(root_text)-1,7},{in->uri,root_text,strlen(in->uri),sizeof(root_text)-1,7}};
    XlspSourceScopeOptions options={&context,documents,2,NULL,0,4096,&config};XlspSourceScope *scope=NULL;CHECK(xlsp_source_scope_capture(&options,&scope)==XR_XIR_BAD_STRUCTURE&&!scope);
    options.count=1;options.maximum_file_bytes=1;CHECK(xlsp_source_scope_capture(&options,&scope)==XR_XIR_BUDGET&&!scope);
    options.maximum_file_bytes=4096;documents[0].text="\x80";documents[0].length=1;CHECK(xlsp_source_scope_capture(&options,&scope)==XR_XIR_BAD_STRUCTURE&&!scope);
    documents[0].text="\0";CHECK(xlsp_source_scope_capture(&options,&scope)==XR_XIR_BAD_STRUCTURE&&!scope);
    config.ignore_pattern_count=-1;CHECK(xlsp_source_scope_capture(&options,&scope)==XR_XIR_BAD_STRUCTURE&&!scope);xr_compile_resources_release(r);CHECK(!runtime_live&&!runtime_bytes);
}
static void renamed_facts(XrJsonValue *result,const Inputs *in) {
    XrJsonValue *changes=xjson_get_array(result,"documentChanges");CHECK(changes&&changes->as.array.count==3);unsigned edits=0,known=0;
    for(int i=0;i<changes->as.array.count;++i){XrJsonValue *change=xjson_array_get(changes,i),*document=xjson_get_object(change,"textDocument"),*version=xjson_get(document,"version"),*items=xjson_get_array(change,"edits");CHECK(document&&version&&items&&items->as.array.count==1);
        if(!strcmp(xjson_get_string(document,"uri"),in->uri)){CHECK(version->is_integer&&xjson_get_int(document,"version")==7);++known;}else CHECK(version->type==XR_JSON_NULL);
        CHECK(!strcmp(xjson_get_string(xjson_array_get(items,0),"newText"),"renamed"));++edits;}
    CHECK(edits==3&&known==1);
}
static XrXirStatus rename_once(const Inputs *in,XrCompileResourceLimits caps,XrCompileResourceStats *stats,size_t fault,bool *injected) {
    XrCompileResources *r=NULL;XrCompileResourceStatus made=xr_compile_resources_new(&caps,&r);if(made!=XR_COMPILE_RESOURCE_OK)return made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrXirCompileContext context={r,xr_xir_compile_default_limits()};XlspConfig config;XlspIgnorePattern patterns[2];configuration(&config,patterns);
    XlspSourceDocument document={in->uri,root_text,strlen(in->uri),sizeof(root_text)-1,7};XlspSourceScopeOptions options={&context,&document,1,NULL,0,4096,&config};XlspSourceScope *scope=NULL;XrJsonValue *result=NULL;
    lsp_json_calls=0;lsp_json_fail=fault;lsp_json_injected=false;XrXirStatus status=xlsp_source_scope_capture(&options,&scope);size_t entry=SIZE_MAX;
    if(status==XR_XIR_OK)status=xlsp_source_scope_find(scope,in->uri,strlen(in->uri),&entry);
    XlspSourceRenameInput input={entry,{1,26},"renamed",7,false,scope?xlsp_source_scope_known_versions(scope):NULL};
    if(status==XR_XIR_OK)status=xlsp_source_rename_json(&context,xlsp_source_scope_documents(scope),xlsp_source_scope_count(scope),&input,&result);
    if(injected)*injected=lsp_json_injected;lsp_json_fail=SIZE_MAX;lsp_json_injected=false;xlsp_source_scope_free(scope);
    if(stats){*stats=sdk_stats(r);if(status!=XR_XIR_OK)fprintf(stderr,"Scope rename status=%u\n",(unsigned)status);}
    xr_compile_resources_release(r);CHECK(!runtime_live&&!runtime_bytes);if(status==XR_XIR_OK)renamed_facts(result,in);else CHECK(!result);xjson_free(result);CHECK(!lsp_json_live&&!lsp_json_bytes);return status;
}
static unsigned destroyed;
static void destroy_document(XrLspDocument *doc){CHECK(doc);doc->uri=NULL;doc->content=NULL;doc->server=NULL;++destroyed;}
static void lens_facts(XrJsonValue *result) {
    CHECK(result&&result->type==XR_JSON_ARRAY&&result->as.array.count==1);
    XrJsonValue *lens=xjson_array_get(result,0),*command=xjson_get_object(lens,"command"),*range=xjson_get_object(lens,"range"),*start=xjson_get_object(range,"start"),*end=xjson_get_object(range,"end");
    CHECK(command&&start&&end&&!strcmp(xjson_get_string(command,"title"),"2 references")&&
        xjson_get_int(start,"line")==0&&xjson_get_int(start,"character")==10&&xjson_get_int(end,"line")==0&&xjson_get_int(end,"character")==16);
}
static XrXirStatus lens_once(const Inputs *in,size_t physical_fault,size_t json_fault,bool *injected) {
    runtime_fail_at=SIZE_MAX;lsp_json_fail=SIZE_MAX;XrLspServer server={0};XlspIgnorePattern patterns[2];configuration(&server.config,patterns);
    server.doc_table=xlsp_document_store_new(destroy_document);CHECK(server.doc_table);destroyed=0;
    char lib_uri[2048];size_t length=strlen(in->uri);CHECK(length>=7&&length<sizeof(lib_uri)&&!strcmp(in->uri+length-7,"root.xr"));memcpy(lib_uri,in->uri,length-7);memcpy(lib_uri+length-7,"lib.xr",7);
    char root[256],library[]="export fn target(value:i64)->i64{return value}\n";strcpy(root,root_text);
    XrLspDocument documents[2]={{0}};const char *uris[]={in->uri,lib_uri},*texts[]={root,library};
    for(unsigned i=0;i<2;++i){documents[i].server=&server;documents[i].uri=(char *)uris[i];documents[i].content=(char *)texts[i];documents[i].length=strlen(texts[i]);documents[i].version=7;
        XlspDocumentInsertion insertion={0};CHECK(xlsp_document_store_prepare(server.doc_table,uris[i],&documents[i],&insertion)&&xlsp_document_store_commit(&insertion));}
    XrJsonValue *params=xjson_new_object(),*document=xjson_new_object();CHECK(params&&document);xjson_object_set_new(document,"uri",xjson_new_string(lib_uri));xjson_object_set_new(params,"textDocument",document);
    XrCompileResources *previous_resources=sdk_ledger(&limits);XrXirCompileContext previous_context={previous_resources,xr_xir_compile_default_limits()};
    XlspSourceDocument previous_documents[]={{in->uri,root_text,strlen(in->uri),sizeof(root_text)-1,7},{lib_uri,library,strlen(lib_uri),sizeof(library)-1,7}};
    CHECK(xlsp_source_workspace_build(&previous_context,previous_documents,2,&server.source_workspace,NULL,NULL)==XR_XIR_OK);xr_compile_resources_release(previous_resources);XlspSourceWorkspace *old=server.source_workspace;CHECK(old);
    XrJsonValue *previous=xlsp_code_lens_handle(&server,params);if(server.source_failure.stage)fprintf(stderr,"Closed CodeLens baseline stage=%u status=%u\n",server.source_failure.stage,server.source_failure.status);CHECK(server.source_failure.stage==0&&server.source_workspace==old);lens_facts(previous);xjson_free(previous);
    runtime_attempts=0;runtime_fail_at=physical_fault;lsp_json_calls=0;lsp_json_fail=json_fault;lsp_json_injected=false;
    XrJsonValue *result=xlsp_code_lens_handle(&server,params);XrXirStatus status=server.source_failure.stage?(XrXirStatus)server.source_failure.status:XR_XIR_OK;
    if(injected)*injected=lsp_json_injected;lsp_json_fail=SIZE_MAX;runtime_fail_at=SIZE_MAX;lsp_json_injected=false;
    CHECK(server.source_workspace==old);if(status!=XR_XIR_OK)CHECK(!result);else CHECK(result);
    xjson_free(params);xlsp_document_store_free(server.doc_table);xlsp_source_workspace_free(server.source_workspace);CHECK(destroyed==2);memset(root,'?',sizeof(root));memset(library,'?',sizeof(library));memset(lib_uri,'?',sizeof(lib_uri));memset(patterns,0,sizeof(patterns));
    CHECK(!runtime_live&&!runtime_bytes);if(status==XR_XIR_OK)lens_facts(result);xjson_free(result);CHECK(!lsp_json_live&&!lsp_json_bytes);return status;
}
static const char preserved_root[]="import { target } from \"./lib\"\nfn local(target:i64)->i64 { return target }\nexport fn call()->i64 { return target() + local(1) }\n";
static const char preserved_library[]="/* \xf0\x9f\x98\x80 */ export fn target()->i64 { return 41 }\n";
static const char preserved_struct[]="/* \xf0\x9f\x98\x80 */ struct S {\n value:i64\n ref bump(x:ref i64)->i64 { this.value += x; return this.value }\n static pick()->i64 { return 1 }\n}\nfn outer()->i64 { return 1 }\n";
static const char preserved_class[]="/* \xf0\x9f\x98\x80 */ final class C {}\n";
static void binding_lens_fact(XrJsonValue *array,int index,int line,int column,int length,const char *title) {
    CHECK(array&&array->type==XR_JSON_ARRAY);XrJsonValue *lens=xjson_array_get(array,index),*range=xjson_get_object(lens,"range"),*start=xjson_get_object(range,"start"),*end=xjson_get_object(range,"end"),*command=xjson_get_object(lens,"command");
    if(start&&end&&command&&strcmp(xjson_get_string(command,"title"),title))fprintf(stderr,"Binding lens index=%d actual=%s expected=%s span=%lld:%lld..%lld:%lld\n",index,xjson_get_string(command,"title"),title,(long long)xjson_get_int(start,"line"),(long long)xjson_get_int(start,"character"),(long long)xjson_get_int(end,"line"),(long long)xjson_get_int(end,"character"));
    CHECK(start&&end&&command&&xjson_get_int(start,"line")==line&&xjson_get_int(end,"line")==line&&xjson_get_int(start,"character")==column&&xjson_get_int(end,"character")==column+length&&
        !strcmp(xjson_get_string(command,"title"),title)&&!strcmp(xjson_get_string(command,"command"),""));
}
static void binding_lenses_facts(XrJsonValue *root,XrJsonValue *lib,unsigned mode) {
    CHECK(root&&root->type==XR_JSON_ARRAY);
    if(mode==0){CHECK(root->as.array.count==2&&lib&&lib->as.array.count==1);binding_lens_fact(root,0,1,3,5,"1 reference");binding_lens_fact(root,1,2,10,4,"0 references");binding_lens_fact(lib,0,0,19,6,"1 reference");}
    else if(mode==1){CHECK(root->as.array.count==4&&!lib);binding_lens_fact(root,0,0,16,1,"0 references");binding_lens_fact(root,1,2,5,4,"0 references");binding_lens_fact(root,2,3,8,4,"0 references");binding_lens_fact(root,3,5,3,5,"0 references");}
    else if(mode==2){CHECK(root->as.array.count==1&&!lib);binding_lens_fact(root,0,0,21,1,"0 references");}
    /* The initializer names the function; the call names the local variable.
     * Reference counts must not invent a call-graph edge through its value. */
    else {CHECK(root->as.array.count==2&&!lib);binding_lens_fact(root,0,0,3,6,"1 reference");binding_lens_fact(root,1,1,3,3,"0 references");}
}
static XrXirStatus binding_lenses_once(const Inputs *in,unsigned mode,XrCompileResourceLimits caps,XrCompileResourceStats *stats,size_t fault,bool *injected) {
    XrCompileResources *r=NULL;XrCompileResourceStatus made=xr_compile_resources_new(&caps,&r);if(made!=XR_COMPILE_RESOURCE_OK)return made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrXirCompileContext context={r,xr_xir_compile_default_limits()};XrJsonValue *root=NULL,*lib=NULL;char uri[2048],library_uri[2048],text[512],library[128];size_t length=strlen(in->uri);CHECK(length>=7&&length<sizeof(uri));
    strcpy(uri,in->uri);memcpy(library_uri,in->uri,length-7);memcpy(library_uri+length-7,"lib.xr",7);strcpy(library,preserved_library);
    strcpy(text,mode==0?preserved_root:mode==1?preserved_struct:mode==2?preserved_class:"fn target()->i64{return 1}\nfn run()->i64{var alias=target;return alias()}\n");
    XlspSourceDocument documents[]={{uri,text,strlen(uri),strlen(text),7},{library_uri,library,strlen(library_uri),strlen(library),7}};
    lsp_json_calls=0;lsp_json_fail=fault;lsp_json_injected=false;
    XrXirStatus status=xlsp_source_binding_lenses_json(&context,documents,mode==0?2:1,0,&root);
    if(status==XR_XIR_OK&&mode==0)status=xlsp_source_binding_lenses_json(&context,documents,2,1,&lib);
    if(status==XR_XIR_OK){XrJsonValue *occupied=root;size_t before=runtime_attempts,json_before=lsp_json_calls;CHECK(xlsp_source_binding_lenses_json(&context,documents,1,0,&occupied)==XR_XIR_BAD_STRUCTURE&&occupied==root&&runtime_attempts==before&&lsp_json_calls==json_before);}
    if(injected)*injected=lsp_json_injected;lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
    if(status!=XR_XIR_OK){xjson_free(root);root=NULL;xjson_free(lib);lib=NULL;}
    if(stats){*stats=sdk_stats(r);if(status!=XR_XIR_OK)fprintf(stderr,"Binding lenses mode=%u status=%u\n",mode,(unsigned)status);}
    memset(uri,'?',sizeof(uri));memset(library_uri,'?',sizeof(library_uri));memset(text,'?',sizeof(text));memset(library,'?',sizeof(library));memset(documents,0,sizeof(documents));
    xr_compile_resources_release(r);CHECK(!runtime_live&&!runtime_bytes);if(status==XR_XIR_OK)binding_lenses_facts(root,lib,mode);else CHECK(!root&&!lib);xjson_free(root);xjson_free(lib);CHECK(!lsp_json_live&&!lsp_json_bytes);return status;
}
static void token_dependency_control(const Inputs *in) {
    char library_uri[2048];size_t length=strlen(in->uri);CHECK(length>=7&&length<sizeof(library_uri));memcpy(library_uri,in->uri,length-7);memcpy(library_uri+length-7,"lib.xr",7);
    const char library[]="export fn target(value:i64)->i64{return value}\n";XlspSourceDocument documents[]={{in->uri,root_text,strlen(in->uri),sizeof(root_text)-1,7},{library_uri,library,strlen(library_uri),sizeof(library)-1,7}};
    XrCompileResources *r=sdk_ledger(&limits);XrXirCompileContext context={r,xr_xir_compile_default_limits()};XlspSourceWorkspace *workspace=NULL;CHECK(xlsp_source_workspace_build(&context,documents,2,&workspace,NULL,NULL)==XR_XIR_OK);
    XrJsonValue *legacy=NULL,*references=NULL;XrXirStatus status=xlsp_source_code_lens_json(r,workspace,library_uri,strlen(library_uri),&legacy);
    XrXirStatus reference_status=xlsp_source_workspace_navigation(workspace,library_uri,strlen(library_uri),(XrLspPosition){0,11},1,false,&references);
    printf("Original qualified-module CodeLens token dependency status=%u output=%s; Source refs status=%u count=%d\n",(unsigned)status,legacy?"JSON":"NULL",(unsigned)reference_status,references?references->as.array.count:-1);
    XlspSourceBindings *bindings=NULL;CHECK(xlsp_source_bindings_build(&context,documents,2,&bindings)==XR_XIR_OK);
    for(size_t root=0;root<2;++root){const XrXirSourceView *v=xlsp_source_bindings_view(bindings,root);const XrXirSourceSyntaxView *syntax=xlsp_source_bindings_syntax(bindings,root);
        for(uint32_t i=0;i<v->declaration_count;++i){const XrXirSourceDeclaration *d=&v->declarations[i];if(d->name&&!strcmp(d->name,"target"))printf("Source target root=%zu id=%u kind=%u parent=%u parentKind=%u role=%u name=%d:%d..%d:%d\n",root,d->id,(unsigned)d->kind,d->parent,d->parent?(unsigned)v->declarations[d->parent-1].kind:UINT32_MAX,(unsigned)syntax->declarations[i].role,syntax->declarations[i].name.line,syntax->declarations[i].name.column,syntax->declarations[i].name.end_line,syntax->declarations[i].name.end_column);}}
    xlsp_source_bindings_free(bindings);bindings=NULL;
    const char function_value[]="fn target()->i64{return 1}\nfn run()->i64{var alias=target;return alias()}\n";documents[0].text=function_value;documents[0].length=sizeof(function_value)-1;
    CHECK(xlsp_source_bindings_build(&context,documents,1,&bindings)==XR_XIR_OK);const XrXirSourceView *v=xlsp_source_bindings_view(bindings,0);const XrXirSourceSyntaxView *syntax=xlsp_source_bindings_syntax(bindings,0);
    for(uint32_t i=0;i<v->declaration_count;++i)printf("Function-value declaration id=%u kind=%u name=%s target=%u\n",v->declarations[i].id,(unsigned)v->declarations[i].kind,v->declarations[i].name,v->declarations[i].target);
    for(uint32_t i=0;i<v->reference_count;++i)printf("Function-value reference binding=%u target=%u access=%u span=%d:%d..%d:%d\n",v->references[i].declaration,v->references[i].target,(unsigned)v->references[i].access,syntax->references[i].line,syntax->references[i].column,syntax->references[i].end_line,syntax->references[i].end_column);
    xlsp_source_bindings_free(bindings);xjson_free(legacy);xjson_free(references);xlsp_source_workspace_free(workspace);xr_compile_resources_release(r);CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes);
}
static int run(int argc,char **argv) {
    (void)sdk_fixture_malloc;(void)sdk_fixture_free;(void)sdk_unlimited;
    CHECK(argc==12||argc==13);Inputs in={argv[1],argv[2],argv[3],argv[4],argv[5],argv[6],argv[7],argv[8],argv[9],argv[10],argv[11]};
    if(argc==13&&!strcmp(argv[12],"--token-control")){token_dependency_control(&in);return 0;}
    bool binding=argc==13&&(!strcmp(argv[12],"--binding-lenses")||!strcmp(argv[12],"--binding-lenses-normal"));
    if(binding){bool full=!strcmp(argv[12],"--binding-lenses");for(unsigned mode=0;mode<4;++mode){XrCompileResourceStats stats={0};runtime_attempts=0;runtime_fail_at=SIZE_MAX;CHECK(binding_lenses_once(&in,mode,limits,&stats,SIZE_MAX,NULL)==XR_XIR_OK);size_t sites=runtime_attempts,json_sites=lsp_json_calls;
            if(full){for(size_t i=0;i<sites;++i){runtime_attempts=0;runtime_fail_at=i;CHECK(binding_lenses_once(&in,mode,limits,NULL,SIZE_MAX,NULL)==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>i);}runtime_fail_at=SIZE_MAX;
                for(size_t i=0;i<json_sites;++i){bool injected=false;CHECK(binding_lenses_once(&in,mode,limits,NULL,i,&injected)==XR_XIR_OUT_OF_MEMORY&&injected);}
                for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){XrCompileResourceLimits exact={stats.allocated_bytes,stats.peak_bytes,stats.work};uint64_t *bound=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;CHECK(*bound&&*bound<INT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);CHECK(binding_lenses_once(&in,mode,exact,NULL,SIZE_MAX,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));}}
            printf("Direct binding CodeLens mode=%u SourceFI=%zu JSONFI=%zu full=%u original3 exact literals/assertions plus function values physical0/0\n",mode,sites,json_sites,full?1u:0u);}return 0;}
    bool lens=argc==13&&(!strcmp(argv[12],"--lens")||!strcmp(argv[12],"--lens-normal"));
    if(lens){bool full=!strcmp(argv[12],"--lens");CHECK(lens_once(&in,SIZE_MAX,SIZE_MAX,NULL)==XR_XIR_OK);size_t sites=runtime_attempts,json_sites=lsp_json_calls;
        if(full){for(size_t i=0;i<sites;++i)CHECK(lens_once(&in,i,SIZE_MAX,NULL)==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>i);
            for(size_t i=0;i<json_sites;++i){bool injected=false;CHECK(lens_once(&in,SIZE_MAX,i,&injected)==XR_XIR_OUT_OF_MEMORY&&injected);}}
        printf("Actual closed-scope CodeLens SourceFI=%zu JSONFI=%zu full=%u repeated request/old owner preservation/2 exact refs physical0/0\n",sites,json_sites,full?1u:0u);return 0;}
    bool rename=argc==13&&(!strcmp(argv[12],"--rename")||!strcmp(argv[12],"--rename-normal"));bool full=argc==12||(rename&&!strcmp(argv[12],"--rename"));CHECK(rename||argc==12||!strcmp(argv[12],"--normal"));
    for(unsigned mode=0;mode<(rename?1u:9u);++mode){XrCompileResourceStats stats={0};runtime_attempts=0;runtime_fail_at=SIZE_MAX;
        CHECK((rename?rename_once(&in,limits,&stats,SIZE_MAX,NULL):once(&in,mode,limits,&stats))==XR_XIR_OK);size_t sites=runtime_attempts,json_sites=lsp_json_calls;
        if(full){for(size_t i=0;i<sites;++i){runtime_attempts=0;runtime_fail_at=i;CHECK((rename?rename_once(&in,limits,NULL,SIZE_MAX,NULL):once(&in,mode,limits,NULL))==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>i);}runtime_fail_at=SIZE_MAX;
            if(rename)for(size_t i=0;i<json_sites;++i){bool injected=false;CHECK(rename_once(&in,limits,NULL,i,&injected)==XR_XIR_OUT_OF_MEMORY&&injected);}
            for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){XrCompileResourceLimits exact={stats.allocated_bytes,stats.peak_bytes,stats.work};uint64_t *bound=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;CHECK(*bound&&*bound<INT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);CHECK((rename?rename_once(&in,exact,NULL,SIZE_MAX,NULL):once(&in,mode,exact,NULL))==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));}}
        printf("Source scope rename=%u mode=%u SourceFI=%zu JSONFI=%zu full=%u closed reverse dependents/overlay/authority/versions physical0/0\n",rename?1u:0u,mode,sites,rename?json_sites:0,full?1u:0u);
    }
    if(!rename){ignores();rejected(&in);}return 0;
}
#if XR_OS_WINDOWS
int wmain(int argc,wchar_t **wide){XrWinPathStatus status;char **argv=xr_win_utf16_arguments(argc,wide,&status);CHECK(argv);int result=run(argc,argv);xr_win_utf8_arguments_free(argc,argv);return result;}
#else
int main(int argc,char **argv){return run(argc,argv);}
#endif
