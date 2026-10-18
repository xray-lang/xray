/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "app/lsp/xlsp_source_inlay.h"
#include "app/lsp/xlsp_source_workspace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_sdk_resource_test.h"
#include "lsp_open_json_owner.h"
#include "app/lsp/xlsp_source_workspace.c"
static const char json_root[]="export struct Box {const value:i64}\nexport fn make()->Box{return Box{value:1}}\nfn main(){}\n";
static const char json_library[]="import { make } from \"./root\"\nexport fn entry(){\n const box=make()\n const count=1\n const values=[box]\n}\n";
static const char json_entry[]="import { entry } from \"./lib\"\nfn main(){entry()}\n";
static const XrLspRange json_range={{0,0},{6,0}};
typedef struct JsonFixture {
    XrCompileResources *resources;
    XlspSourceSnapshot *snapshots[3];
    XlspSourceDocument document;
} JsonFixture;
static void json_fixture_free(JsonFixture *fixture) {
    for(size_t i=0;i<3;++i)xlsp_source_snapshot_free(fixture->snapshots[i]);
    xr_compile_resources_release(fixture->resources);memset(fixture,0,sizeof(*fixture));
}
static XlspSourceWorkspace *json_workspace(XrCompileResources *resources,char **uris,const char *root,int64_t version,bool reorder) {
    XlspSourceDocument documents[]={{uris[0],root,strlen(uris[0]),strlen(root),version},
        {uris[1],json_library,strlen(uris[1]),sizeof(json_library)-1,7},{uris[2],json_entry,strlen(uris[2]),sizeof(json_entry)-1,8}};
    if(reorder){XlspSourceDocument first=documents[0];documents[0]=documents[2];documents[2]=documents[1];documents[1]=first;}
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};XlspSourceWorkspace *workspace=NULL;
    unsigned stage=0;XlspSourceWorkspaceFailure failure={0};
    XrXirStatus status=xlsp_source_workspace_build(&context,documents,3,&workspace,&stage,&failure);
    if(status!=XR_XIR_OK)fprintf(stderr,"type JSON workspace stage=%u status=%u %s\n",stage,status,failure.diagnostic.message);
    CHECK(status==XR_XIR_OK);xlsp_source_workspace_failure_free(&failure);return workspace;
}
static JsonFixture json_fixture(char **uris) {
    const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    CHECK(limits.work<sdk_unlimited.work);JsonFixture fixture={0};fixture.resources=sdk_ledger(&limits);
    XlspSourceWorkspace *workspace=json_workspace(fixture.resources,uris,json_root,9,false);
    fixture.document=(XlspSourceDocument){uris[1],json_library,strlen(uris[1]),sizeof(json_library)-1,7};
    for(size_t i=0;i<3;++i){fixture.snapshots[i]=workspace->snapshots[i];workspace->snapshots[i]=NULL;}
    xlsp_source_workspace_free(workspace);return fixture;
}
static void json_check(const XrJsonValue *value) {
    CHECK(value&&value->type==XR_JSON_ARRAY&&value->as.array.count==3);
    const char *labels[]={": Box",": i64",": Array<Box>"};const unsigned columns[]={10,12,13};
    for(int i=0;i<3;++i) {
        XrJsonValue *item=value->as.array.items[i];CHECK(item->type==XR_JSON_OBJECT&&item->as.object.count==4);
        CHECK(!strcmp(xjson_get_string(item,"label"),labels[i])&&xjson_get_int(item,"kind")==1&&xjson_get_bool(item,"paddingLeft"));
        XrJsonValue *position=xjson_get_object(item,"position");CHECK(position&&position->as.object.count==2);
        CHECK(xjson_get_int(position,"line")==i+2&&xjson_get_int(position,"character")==columns[i]);
    }
}
static XlspSourceTypeHintRequest json_request(JsonFixture *fixture) {
    return (XlspSourceTypeHintRequest){fixture->resources,fixture->snapshots,3,fixture->document,json_range};
}
static void json_conflicts(JsonFixture *fixture,char **uris) {
    const char *changed="export struct Box {const value:bool}\nexport fn make()->Box{return Box{value:true}}\nfn main(){}\n";
    for(unsigned mode=0;mode<2;++mode) {
        XlspSourceWorkspace *workspace=json_workspace(fixture->resources,uris,mode?changed:json_root,mode?9:10,false);
        XlspSourceSnapshot *snapshots[]={fixture->snapshots[1],workspace->snapshots[2]};
        XlspSourceTypeHintRequest request={fixture->resources,snapshots,2,fixture->document,json_range};XrJsonValue *output=NULL;
        size_t blocks=runtime_live,bytes=runtime_bytes,json_blocks=lsp_json_live,json_bytes=lsp_json_bytes;
        XrXirStatus status=xlsp_source_type_hints_many_json(&request,&output);
        if(status!=XR_XIR_BAD_STRUCTURE)fprintf(stderr,"TYPE_PROVENANCE accepted conflict=%s status=%u\n",mode?"fingerprint":"version",status);
        CHECK(status==XR_XIR_BAD_STRUCTURE&&!output);
        CHECK(runtime_live==blocks&&runtime_bytes==bytes&&lsp_json_live==json_blocks&&lsp_json_bytes==json_bytes);
        xlsp_source_workspace_free(workspace);
    }
}
static void json_reordered(JsonFixture *fixture,char **uris) {
    XlspSourceWorkspace *workspace=json_workspace(fixture->resources,uris,json_root,9,true);
    XlspSourceSnapshot *snapshots[]={fixture->snapshots[1],workspace->snapshots[0]};
    XlspSourceTypeHintRequest request={fixture->resources,snapshots,2,fixture->document,json_range};XrJsonValue *out=NULL;
    CHECK(xlsp_source_type_hints_many_json(&request,&out)==XR_XIR_OK);json_check(out);xjson_free(out);
    xlsp_source_workspace_free(workspace);
}
static void json_invalid(JsonFixture *fixture,char **uris) {
    XlspSourceTypeHintRequest request=json_request(fixture);XrJsonValue *output=NULL;
    request.range=(XrLspRange){{3,12},{3,12}};CHECK(xlsp_source_type_hints_many_json(&request,&output)==XR_XIR_OK);
    CHECK(output&&output->type==XR_JSON_ARRAY&&!output->as.array.count);xjson_free(output);output=NULL;
    request=json_request(fixture);request.range.end.character=99;
    CHECK(xlsp_source_type_hints_many_json(&request,&output)==XR_XIR_BAD_STRUCTURE&&!output);
    request=json_request(fixture);request.snapshots=fixture->snapshots;request.count=1;
    CHECK(xlsp_source_type_hints_many_json(&request,&output)==XR_XIR_UNRESOLVED&&!output);
    XrCompileResources *foreign=sdk_ledger(&sdk_unlimited);XlspSourceWorkspace *workspace=json_workspace(foreign,uris,json_root,9,false);
    XlspSourceSnapshot *snapshots[]={workspace->snapshots[0]};request=json_request(fixture);request.snapshots=snapshots;request.count=1;
    XrCompileResourceStats before=sdk_stats(foreign);
    CHECK(xlsp_source_type_hints_many_json(&request,&output)==XR_XIR_BAD_STRUCTURE&&!output);
    CHECK(sdk_stats(foreign).work==before.work);xlsp_source_workspace_free(workspace);xr_compile_resources_release(foreign);
    request=json_request(fixture);--request.document.version;
    CHECK(xlsp_source_type_hints_many_json(&request,&output)==XR_XIR_BAD_STRUCTURE&&!output);
    request=json_request(fixture);request.range=(XrLspRange){{0,0},{0,0}};--request.document.version;
    CHECK(xlsp_source_type_hints_many_json(&request,&output)==XR_XIR_BAD_STRUCTURE&&!output);
    request=json_request(fixture);output=xjson_new_null();CHECK(output);XrJsonValue *unchanged=output;
    CHECK(xlsp_source_type_hints_many_json(&request,&output)==XR_XIR_BAD_STRUCTURE&&output==unchanged);xjson_free(output);
}
static void json_faults(JsonFixture *fixture,bool failures,size_t *source_sites,size_t *json_sites) {
    XlspSourceTypeHintRequest request=json_request(fixture);
    for(unsigned domain=0;domain<2;++domain) {
        size_t sites=0;
        for(size_t probe=0;probe<=sites;++probe) {
            if(probe&&!failures)break;
            size_t source_start=runtime_attempts,json_start=lsp_json_calls,blocks=runtime_live,bytes=runtime_bytes;
            uint64_t live=sdk_stats(fixture->resources).live_bytes;XrJsonValue *output=NULL;
            if(probe) {if(domain)lsp_json_fail=json_start+probe-1;else runtime_fail_at=source_start+probe-1;}
            XrXirStatus status=xlsp_source_type_hints_many_json(&request,&output);runtime_fail_at=lsp_json_fail=SIZE_MAX;
            if(!probe){CHECK(status==XR_XIR_OK);json_check(output);sites=domain?lsp_json_calls-json_start:runtime_attempts-source_start;CHECK(sites);}
            else CHECK(status==XR_XIR_OUT_OF_MEMORY&&!output);
            xjson_free(output);CHECK(runtime_live==blocks&&runtime_bytes==bytes&&sdk_stats(fixture->resources).live_bytes==live);
            CHECK(!lsp_json_live&&!lsp_json_bytes);
        }
        if(domain)*json_sites=sites;else *source_sites=sites;
    }
}
int main(int argc,char **argv) {
    CHECK(argc==4||(argc==5&&!strcmp(argv[4],"--resources")));bool failures=argc==5;
    JsonFixture fixture=json_fixture(argv+1);
    XlspSourceWorkspace absent={0};absent.context.resources=fixture.resources;absent.snapshots=fixture.snapshots;absent.count=1;
    XlspSourceInlayOptions options={json_range,true,true};XrJsonValue *missing=NULL;
    CHECK(xlsp_source_workspace_inlay_bindings(&absent,argv[2],strlen(argv[2]),&options,&missing)==XR_XIR_UNRESOLVED&&!missing);json_conflicts(&fixture,argv+1);json_reordered(&fixture,argv+1);json_invalid(&fixture,argv+1);
    size_t source=0,json=0;json_faults(&fixture,failures,&source,&json);
    XlspSourceTypeHintRequest request=json_request(&fixture);XrJsonValue *output=NULL;
    CHECK(xlsp_source_type_hints_many_json(&request,&output)==XR_XIR_OK);json_fixture_free(&fixture);
    json_check(output);xjson_free(output);CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes);
    printf("Type JSON source=%zu json=%zu full_FI=%u roots=2 conflicts=2 invalid=7 producer_dead=1 physical0/0 JSON0/0\n",source,json,failures?1u:0u);return 0;
}
