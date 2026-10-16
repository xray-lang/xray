/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "app/lsp/xlsp_source_inlay.h"
#include "app/lsp/xlsp_source_workspace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_sdk_resource_test.h"
#include "app/lsp/xlsp_source_workspace.c"

static XlspSourceSnapshot *hints_producer(XrCompileResources *resources,const char *uri,const char *program) {
    size_t length=strlen(program);char *text=xr_malloc(length+1);CHECK(text);memcpy(text,program,length+1);
    size_t uri_length=strlen(uri);char *owned_uri=xr_malloc(uri_length+1);CHECK(owned_uri);memcpy(owned_uri,uri,uri_length+1);
    XlspSourceDocument input={owned_uri,text,uri_length,length,9};
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};XlspSourceWorkspace *workspace=NULL;
    unsigned stage=0;XlspSourceWorkspaceFailure failure={0};
    XrXirStatus status=xlsp_source_workspace_build(&context,&input,1,&workspace,&stage,&failure);
    if(status!=XR_XIR_OK)fprintf(stderr,"hints stage=%u status=%u %s\n%s",stage,status,failure.diagnostic.message,program);
    CHECK(status==XR_XIR_OK&&workspace);
    XlspSourceSnapshot *snapshot=workspace->snapshots[0];workspace->snapshots[0]=NULL;
    xlsp_source_workspace_free(workspace);xlsp_source_workspace_failure_free(&failure);
    memset(text,'?',length);memset(owned_uri,'?',uri_length);xr_free(text);xr_free(owned_uri);return snapshot;
}
static void hints_check(const XlspSourceTypeHints *hints) {
    CHECK(hints->count==3);
    const char *labels[]={": Actual",": i64",": Array<Actual>"};
    const unsigned rows[]={1,3,4},columns[]={15,12,13};
    for(size_t i=0;i<3;++i) {
        CHECK(!strcmp(hints->items[i].label,labels[i]));CHECK(hints->items[i].label_length==strlen(labels[i]));
        CHECK(hints->items[i].position.line==rows[i]&&hints->items[i].position.character==columns[i]);
    }
}
static void hints_invalid(const XlspSourceSnapshot *snapshot,const char *uri) {
    const XrLspRange ranges[]={{{4,0},{1,0}},{{1,0},{1,99}},{{99,0},{99,0}}};
    for(size_t i=0;i<sizeof(ranges)/sizeof(ranges[0]);++i) {
        XlspSourceTypeHints hints={0};size_t blocks=runtime_live,bytes=runtime_bytes;
        CHECK(xlsp_source_type_hints(snapshot,uri,strlen(uri),ranges[i],&hints)!=XR_XIR_OK);
        CHECK(!hints.items&&!hints.count&&runtime_live==blocks&&runtime_bytes==bytes);
    }
    XlspSourceTypeHints hints={0};XrLspRange empty={{1,15},{1,15}};
    CHECK(xlsp_source_type_hints(snapshot,uri,strlen(uri),empty,&hints)==XR_XIR_OK&&!hints.count&&!hints.items);
    XrLspRange selected={{1,15},{3,12}};
    CHECK(xlsp_source_type_hints(snapshot,uri,strlen(uri),selected,&hints)==XR_XIR_OK&&hints.count==1);
    CHECK(!strcmp(hints.items[0].label,": Actual"));xlsp_source_type_hints_free(&hints);
    hints.count=1;CHECK(xlsp_source_type_hints(snapshot,uri,strlen(uri),selected,&hints)==XR_XIR_BAD_STRUCTURE&&hints.count==1);
}
static size_t hints_attachment(XrCompileResources *resources,const XlspSourceSnapshot *snapshot,const char *uri,bool failures) {
    const XrLspRange range={{0,0},{8,0}};size_t sites=0;
    for(size_t probe=0;probe<=sites;++probe) {
        if(probe&&!failures)break;
        XlspSourceTypeHints hints={0};size_t blocks=runtime_live,bytes=runtime_bytes,start=runtime_attempts;
        uint64_t live=sdk_stats(resources).live_bytes;
        runtime_fail_at=probe?start+probe-1:SIZE_MAX;
        XrXirStatus status=xlsp_source_type_hints(snapshot,uri,strlen(uri),range,&hints);runtime_fail_at=SIZE_MAX;
        if(!probe){CHECK(status==XR_XIR_OK);sites=runtime_attempts-start;CHECK(sites);hints_check(&hints);}
        else CHECK(status==XR_XIR_OUT_OF_MEMORY&&!hints.items&&!hints.count);
        xlsp_source_type_hints_free(&hints);CHECK(runtime_live==blocks&&runtime_bytes==bytes);
        CHECK(sdk_stats(resources).live_bytes==live);
    }
    return sites;
}
static void hints_unicode(XrCompileResources *resources,const char *uri) {
    XlspSourceSnapshot *snapshot=hints_producer(resources,uri,"fn main(){const emoji=\"\xF0\x9F\x98\x80\";const after=1}\n");
    XlspSourceTypeHints hints={0};XrLspRange range={{0,0},{1,0}};
    CHECK(xlsp_source_type_hints(snapshot,uri,strlen(uri),range,&hints)==XR_XIR_OK&&hints.count==2);
    CHECK(!strcmp(hints.items[0].label,": string")&&hints.items[0].position.character==21);
    CHECK(!strcmp(hints.items[1].label,": i64")&&hints.items[1].position.character==38);
    xlsp_source_type_hints_free(&hints);range.start.character=24;
    CHECK(xlsp_source_type_hints(snapshot,uri,strlen(uri),range,&hints)==XR_XIR_BAD_STRUCTURE&&!hints.items&&!hints.count);
    xlsp_source_snapshot_free(snapshot);
}
static void hints_long(XrCompileResources *resources,const char *uri) {
    char program[128]="fn main(){const deeply=";size_t used=strlen(program);char expected[160]=": ";size_t count=2;
    for(unsigned i=0;i<18;++i){program[used++]='[';memcpy(expected+count,"Array<",6);count+=6;}
    program[used++]='1';memcpy(expected+count,"i64",3);count+=3;
    for(unsigned i=0;i<18;++i){program[used++]=']';expected[count++]='>';}
    memcpy(program+used,"}\n",3);expected[count]=0;
    XlspSourceSnapshot *snapshot=hints_producer(resources,uri,program);XlspSourceTypeHints hints={0};XrLspRange range={{0,0},{1,0}};
    CHECK(xlsp_source_type_hints(snapshot,uri,strlen(uri),range,&hints)==XR_XIR_OK&&hints.count==1);
    CHECK(hints.items[0].label_length==count&&count>64&&!strcmp(hints.items[0].label,expected));
    xlsp_source_snapshot_free(snapshot);CHECK(!strcmp(hints.items[0].label,expected));xlsp_source_type_hints_free(&hints);
}
int main(int argc,char **argv) {
    CHECK(argc==2||(argc==3&&!strcmp(argv[2],"--resources")));bool failures=argc==3;
    XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    CHECK(limits.work<sdk_unlimited.work);XrCompileResources *resources=sdk_ledger(&limits);
    XlspSourceSnapshot *snapshot=hints_producer(resources,argv[1],
        "fn wrapper<Actual>(value:Actual)->Actual {\n const inferred=value\n const declared:Actual=value\n"
        " var changed=1\n const values=[value]\n return inferred\n}\nfn main()->i64{return wrapper(1)}\n");
    hints_invalid(snapshot,argv[1]);size_t sites=hints_attachment(resources,snapshot,argv[1],failures);
    XlspSourceHover hover={0};CHECK(xlsp_source_hover(snapshot,argv[1],strlen(argv[1]),(XrLspPosition){1,8},&hover)==XR_XIR_OK);
    CHECK(!strcmp(hover.text,"const inferred: Actual"));xlsp_source_hover_free(&hover);
    XlspSourceTypeHints hints={0};XrLspRange range={{0,0},{8,0}};
    CHECK(xlsp_source_type_hints(snapshot,argv[1],strlen(argv[1]),range,&hints)==XR_XIR_OK);hints_check(&hints);
    xlsp_source_snapshot_free(snapshot);hints_unicode(resources,argv[1]);hints_long(resources,argv[1]);
    xr_compile_resources_release(resources);hints_check(&hints);xlsp_source_type_hints_free(&hints);
    CHECK(!runtime_live&&!runtime_bytes);
    printf("Type hints sites=%zu full_FI=%u generic=1 unicode=1 long=1 producer_dead=1 physical0/0\n",sites,failures?1u:0u);return 0;
}
