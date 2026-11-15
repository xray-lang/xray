/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "app/lsp/xlsp_source_workspace.h"
#include "api/xsource_compile.h"
#include "xir/xxir_compile_memory.h"
#include "xir/xxir_source_query_internal.h"
#include "xir/xxir_construction.h"
#include "xir/xxir_types.h"
#include "shared/xr_param_mode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"

static XrLspPosition position(const char *text,const char *needle) {
    const char *found=strstr(text,needle);CHECK(found);
    XrLspPosition result={0};
    for(const char *at=text;at<found;++at) {
        if(*at=='\n'){++result.line;result.character=0;}
        else ++result.character;
    }
    return result;
}
static void signature(XlspSourceWorkspace *workspace,const char *uri,const char *text,
    const char *needle,int expected,const char *label) {
    XrJsonValue *out=NULL;
    XrXirStatus status=xlsp_source_workspace_navigation(workspace,uri,strlen(uri),position(text,needle),4,false,&out);
    if(status!=XR_XIR_OK)fprintf(stderr,"signature needle=%s status=%d source=%s\n",needle,status,text);
    CHECK(status==XR_XIR_OK);
    CHECK(out&&xjson_get_int_or(out,"activeParameter",-1)==expected);
    XrJsonValue *items=xjson_get_array(out,"signatures");CHECK(xjson_array_len(items)==1);
    const char *actual=xjson_get_string(xjson_array_get(items,0),"label");CHECK(actual);
    if(label)CHECK(strcmp(actual,label)==0);
    xjson_free(out);
}
static XrJsonValue *hints(XlspSourceWorkspace *workspace,const char *uri,const char *text) {
    XrLspPosition end={0};
    for(const char *at=text;*at;++at) {
        if(*at=='\n'){++end.line;end.character=0;}else ++end.character;
    }
    XlspSourceInlayOptions options={.range={{0,0},end},.parameters=true};
    XrJsonValue *out=NULL;
    CHECK(xlsp_source_workspace_inlay_bindings(workspace,uri,strlen(uri),&options,&out)==XR_XIR_OK);
    return out;
}
static void basic(XlspSourceWorkspace *workspace,const char *uri,const char *text) {
    const char *label="fn mix(left: i64, right: i64, tail: i64) -> i64";
    signature(workspace,uri,text,"right:22",1,label);
    signature(workspace,uri,text,"left:11",0,label);
    signature(workspace,uri,text,"tail:9",2,label);
    signature(workspace,uri,text,"right:4",1,label);
    signature(workspace,uri,text,"5,6",0,label);
    signature(workspace,uri,text,"6)",1,label);
    signature(workspace,uri,text,"...(7,8)",-1,label);
    XrJsonValue *out=hints(workspace,uri,text);CHECK(xjson_array_len(out)==3);
    const char *expected[]={"left:","left:","right:"};
    for(int i=0;i<3;++i)CHECK(strcmp(xjson_get_string(xjson_array_get(out,i),"label"),expected[i])==0);
    xjson_free(out);
}
static void receiver(XlspSourceWorkspace *workspace,const char *uri,const char *text) {
    const char *label="fn Box.add(other: i64, extra: i64) -> i64";
    signature(workspace,uri,text,"extra:8",1,label);
    signature(workspace,uri,text,"other:4",0,label);
    XrJsonValue *out=hints(workspace,uri,text);CHECK(xjson_array_len(out)==0);xjson_free(out);
}
static void reference(XlspSourceWorkspace *workspace,const char *uri,const char *text) {
    const char *label="fn tweak(value: ref i64, delta: i64) -> ()";
    signature(workspace,uri,text,"delta:2",1,label);
    signature(workspace,uri,text,"value:ref x",0,label);
    XrJsonValue *out=hints(workspace,uri,text);CHECK(xjson_array_len(out)==0);xjson_free(out);
}
static void native(XlspSourceWorkspace *workspace,const char *uri,const char *text) {
    signature(workspace,uri,text,"end:2",2,NULL);
    signature(workspace,uri,text,"start:0",1,NULL);
    signature(workspace,uri,text,"end:3",1,NULL);
    signature(workspace,uri,text,"start:1",0,NULL);
    XrJsonValue *out=hints(workspace,uri,text);CHECK(xjson_array_len(out)==0);xjson_free(out);
}
static void run_case(const char *uri,const char *source,
    void (*verify)(XlspSourceWorkspace *,const char *,const char *)) {
    XrCompileResources *resources=sdk_ledger(&sdk_unlimited);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrCompileResourceStats baseline=sdk_stats(resources);
    size_t text_bytes=strlen(source)+1;char *text=xr_malloc(text_bytes);CHECK(text);memcpy(text,source,text_bytes);
    XlspSourceDocument document={uri,text,strlen(uri),text_bytes-1,1};
    XlspSourceWorkspace *workspace=NULL;unsigned stage=0;XlspSourceWorkspaceFailure failure={0};
    XrXirStatus status=xlsp_source_workspace_build(&context,&document,1,&workspace,&stage,&failure,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"workspace status=%d stage=%u diagnostic=%s\n",status,stage,failure.diagnostic.message);
    CHECK(status==XR_XIR_OK&&workspace);xlsp_source_workspace_failure_free(&failure);
    memset(text,'?',text_bytes);xr_free(text);text=NULL;memset(&document,0,sizeof(document));
    verify(workspace,uri,source);
    XrJsonValue *retained=hints(workspace,uri,source);
    xlsp_source_workspace_free(workspace);
    CHECK(sdk_stats(resources).held_bytes==baseline.held_bytes);
    xr_compile_resources_release(resources);
    CHECK(!runtime_live&&!runtime_bytes);CHECK(retained&&retained->type==XR_JSON_ARRAY);xjson_free(retained);
}
static void captured(XlspSourceWorkspace *workspace,const char *uri,const char *text) {
    const char *label="fn add(a: i64, b: i64) -> i64";
    signature(workspace,uri,text,"b:4",1,label);
    signature(workspace,uri,text,"a:3",0,label);
    XrJsonValue *definition=NULL;
    CHECK(xlsp_source_workspace_navigation(workspace,uri,strlen(uri),position(text,"add(b:4"),0,false,&definition)==XR_XIR_OK);
    CHECK(definition&&xjson_get_string(definition,"uri")&&strcmp(xjson_get_string(definition,"uri"),uri)==0);
    XrJsonValue *range=xjson_get_object(definition,"range");
    XrJsonValue *start=xjson_get_object(range,"start"),*end=xjson_get_object(range,"end");
    CHECK(start&&end&&xjson_get_int(start,"line")==2&&xjson_get_int(start,"character")==3&&
        xjson_get_int(end,"line")==2&&xjson_get_int(end,"character")==6);
    xjson_free(definition);
    XrJsonValue *out=hints(workspace,uri,text);CHECK(xjson_array_len(out)==0);xjson_free(out);
}
static void captured_generator(XlspSourceWorkspace *workspace,const char *uri,const char *text) {
    const char *label="fn values(a: i64, b: i64) -> Iterator<i64>";
    signature(workspace,uri,text,"b:4",1,label);
    signature(workspace,uri,text,"a:3",0,label);
    XrJsonValue *definition=NULL;
    CHECK(xlsp_source_workspace_navigation(workspace,uri,strlen(uri),position(text,"values(b:4"),0,false,&definition)==XR_XIR_OK);
    CHECK(definition&&xjson_get_string(definition,"uri")&&strcmp(xjson_get_string(definition,"uri"),uri)==0);
    XrJsonValue *range=xjson_get_object(definition,"range");
    XrJsonValue *start=xjson_get_object(range,"start"),*end=xjson_get_object(range,"end");
    CHECK(start&&end&&xjson_get_int(start,"line")==2&&xjson_get_int(start,"character")==3&&
        xjson_get_int(end,"line")==2&&xjson_get_int(end,"character")==9);
    xjson_free(definition);
    XrJsonValue *out=hints(workspace,uri,text);CHECK(xjson_array_len(out)==0);xjson_free(out);
}
static void iterator_error(XlspSourceWorkspace *workspace,const char *uri,const char *text) {
    signature(workspace,uri,text,"value:value",0,
        "fn carry(value: Iterator<i64, Failure>) -> Iterator<i64, Failure>");
    XrJsonValue *out=hints(workspace,uri,text);CHECK(xjson_array_len(out)==0);xjson_free(out);
}
static size_t display_owner(size_t failure_site,bool malformed) {
    runtime_fail_at=SIZE_MAX;
    XrCompileResources *resources=sdk_ledger(&sdk_unlimited);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrCompileResourceStats baseline=sdk_stats(resources);
    char *name=xr_malloc(sizeof("actual")),*other=xr_malloc(sizeof("second"));CHECK(name&&other);
    memcpy(name,"actual",sizeof("actual"));memcpy(other,"second",sizeof("second"));
    const char *names[]={NULL,name,other};uint32_t modes[]={XR_PARAM_READ,XR_PARAM_REF,XR_PARAM_MOVE};
    XrXirSourceType parameters[]={{XR_XIR_I64,0,true},{XR_XIR_I64,0,true},{XR_XIR_I64,0,true}};
    XrXirSourceQueryModule module={"display::module","fixture.xr",{{0}}};
    XrXirSourceDeclaration declaration={.id=1,.kind=XR_XIR_SOURCE_FUNCTION,.name="fixture",
        .parameters=parameters,.parameter_count=3,.parameter_names=names,.parameter_modes=malformed?NULL:modes};
    XrXirTypes types={0};XrXirConstructionSpec spec={0};XrXirSlotFacts slots={0};
    XrXirConstruction *construction=NULL;
    CHECK(xr_xir_compile_construction_new_v6(&context,&types,&spec,&slots,&construction,NULL)==XR_XIR_OK);
    XrXirSourceView view={.modules=&module,.module_count=1,.declarations=&declaration,.declaration_count=1,.types=&types};
    XrXirSourceSnapshot *snapshot=NULL;size_t first=runtime_attempts;
    if(failure_site)runtime_fail_at=first+failure_site-1;
    XrXirStatus status=xr_xir_compile_source_snapshot_copy_v2(&context,&view,construction,&snapshot);
    size_t attempts=runtime_attempts-first;runtime_fail_at=SIZE_MAX;
    xr_xir_compile_construction_free(construction);
    memset(name,'?',sizeof("actual"));memset(other,'?',sizeof("second"));xr_free(name);xr_free(other);
    if(malformed)CHECK(status==XR_XIR_BAD_STRUCTURE&&!snapshot);
    else if(failure_site)CHECK(status==XR_XIR_OUT_OF_MEMORY&&!snapshot&&attempts>=failure_site);
    else {
        CHECK(status==XR_XIR_OK&&snapshot);
        modes[1]=XR_PARAM_MOVE;
        const XrXirSourceDeclaration *owned=xr_xir_compile_source_snapshot_view(snapshot)->declarations;
        CHECK(owned->parameter_names!=names&&owned->parameter_modes!=modes&&!owned->parameter_names[0]);
        CHECK(strcmp(owned->parameter_names[1],"actual")==0&&strcmp(owned->parameter_names[2],"second")==0);
        CHECK(owned->parameter_modes[1]==XR_PARAM_REF&&owned->parameter_modes[2]==XR_PARAM_MOVE);
    }
    xr_xir_compile_source_snapshot_free(snapshot);
    CHECK(sdk_stats(resources).held_bytes==baseline.held_bytes);
    xr_compile_resources_release(resources);
    CHECK(!runtime_live&&!runtime_bytes);return attempts;
}
int main(int argc,char **argv) {
    CHECK(argc==2);
    size_t sites=display_owner(0,false);CHECK(sites);
    for(size_t site=1;site<=sites;++site)(void)display_owner(site,false);
    (void)display_owner(0,true);
    run_case(argv[1],
        "fn mix(left:i64,right:i64,tail:i64=7)->i64{return left+right+tail}\n"
        "var x=mix(right:22,left:11)\n"
        "var y=mix(3,tail:9,right:4)\n"
        "var z=mix(5,6)\n"
        "var t=mix(...(7,8))\n",basic);
    run_case(argv[1],
        "struct Box {\nvalue:i64\nadd(other:i64,extra:i64=0)->i64{return this.value+other+extra}\n}\n"
        "var box=Box{value:1}\nvar result=box.add(extra:8,other:4)\n",receiver);
    run_case(argv[1],
        "fn tweak(value:ref i64,delta:i64){value+=delta}\nvar x=1\ntweak(delta:2,value:ref x)\n",reference);
    run_case(argv[1],
        "var data:Array<i64>=[1,2]\nvar filled=data.fill(value:9,end:2,start:0)\n"
        "var sliced=\"abcd\".slice(end:3,start:1)\n",native);
    run_case(argv[1],
        "fn outer()->i64{\nvar base=7\nfn add(a:i64,b:i64=2)->i64{return base+a+b}\n"
        "return add(b:4,a:3)\n}\nvar result=outer()\n",captured);
    run_case(argv[1],
        "fn outer()->Iterator<i64>{\nvar base=7\nfn values(a:i64,b:i64=2)->Iterator<i64>{yield base+a+b}\n"
        "return values(b:4,a:3)\n}\nconst result=outer()\n",captured_generator);
    run_case(argv[1],
        "enum Failure { Bad }\nfn carry(value:Iterator<i64,Failure>)->Iterator<i64,Failure>{return value}\n"
        "fn use(value:Iterator<i64,Failure>){const out=carry(value:value)}\n",iterator_error);
    puts("LSP named arguments: reordered activeParameter, labelled hint suppression, receiver/ref/native, spread unknown, producer death PASS physical0/0");
    return 0;
}
