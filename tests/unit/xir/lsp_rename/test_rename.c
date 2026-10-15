/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_source_rename.h"
#include "app/lsp/xlsp_source_bindings.h"
#include "base/xwindows_utf8.h"
#include "os/os_fs.h"
#include "base/xfileio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"
#include "lsp_open_json_owner.h"
static const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
static const char *const texts[]={
    "fn first(value:i64)->i64{return value+value}\nfn second(value:i64)->i64{return value}\n",
    "var counter = 0\n",
    "fn target()->i64{return 1}\nfn run()->i64{var alias=target;return alias()}\n",
    "import \"./lib\" as lib\nfn run()->i64{return lib.target(1)}\n",
    "import \"./closed\" as lib\nfn run()->i64{return lib.target(1)}\n",
    "/* \xf0\x9f\x98\x80 */ fn run(value:i64)->i64{\r\nreturn value+value\r\n}\r\n",
    "fn first(value:i64)->i64{return value+value}\nfn second(value:i64)->i64{return value}\n",
    "/* \xf0\x9f\x98\x80 */ fn run(value:i64)->i64{\r\nreturn value+value\r\n}\r\n"
};
static void facts(XrJsonValue *result,const char *uri,unsigned mode) {
    CHECK(result&&result->type==XR_JSON_OBJECT);
    if(mode==1||mode==7){XrJsonValue *range=xjson_get_object(result,"range");CHECK(range);CHECK(!strcmp(xjson_get_string(result,"placeholder"),mode==1?"counter":"value"));
        XrJsonValue *start=xjson_get_object(range,"start");CHECK(xjson_get_int(start,"line")== (mode==1?0:1)&&xjson_get_int(start,"character")== (mode==1?4:7));return;}
    XrJsonValue *changes=xjson_get_array(result,"documentChanges");CHECK(changes);if(mode==6){CHECK(!changes->as.array.count);return;}int total=0;
    for(int i=0;i<changes->as.array.count;++i){XrJsonValue *document=xjson_array_get(changes,i),*identity=xjson_get_object(document,"textDocument"),*edits=xjson_get_array(document,"edits");CHECK(identity&&edits);
        const char *document_uri=xjson_get_string(identity,"uri");bool known=!(mode==4&&strcmp(document_uri,uri));
        if(known)CHECK(xjson_get(identity,"version")->is_integer&&xjson_get_int(identity,"version")== (mode==5?INT32_MAX:7));
        else CHECK(xjson_get(identity,"version")->type==XR_JSON_NULL);
        if(mode!=3&&mode!=4)CHECK(!strcmp(document_uri,uri));
        for(int j=0;j<edits->as.array.count;++j){XrJsonValue *edit=xjson_array_get(edits,j);CHECK(!strcmp(xjson_get_string(edit,"newText"),mode==2?"callback":mode==5?"fresh\xf0\x9f\x98\x80":"changed"));
            XrJsonValue *range=xjson_get_object(edit,"range"),*start=xjson_get_object(range,"start"),*end=xjson_get_object(range,"end");CHECK(start&&end);
            if(mode==0){int columns[]={38,32,9};CHECK(xjson_get_int(start,"line")==0&&xjson_get_int(start,"character")==columns[j]&&xjson_get_int(end,"character")==columns[j]+5);}
            if(mode==2){int columns[]={38,18};CHECK(xjson_get_int(start,"line")==1&&xjson_get_int(start,"character")==columns[j]&&xjson_get_int(end,"character")==columns[j]+5);}
            if(mode==5){int lines[]={1,1,0},columns[]={13,7,16};CHECK(xjson_get_int(start,"line")==lines[j]&&xjson_get_int(start,"character")==columns[j]&&xjson_get_int(end,"character")==columns[j]+5);}
            ++total;}}
    CHECK(total==((mode==0||mode==5)?3:2));
}
static XrXirStatus once(const char *uri,const char *lib,const char *closed,unsigned mode,XrCompileResourceLimits caps,XrCompileResourceStats *stats,size_t json_fault,bool *injected) {
    XrCompileResources *r=NULL;XrCompileResourceStatus made=xr_compile_resources_new(&caps,&r);if(made!=XR_COMPILE_RESOURCE_OK)return made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrXirCompileContext context={r,xr_xir_compile_default_limits()};XrJsonValue *result=NULL;
    char text[512],root[1024],library[1024],child[]="export fn target(value:i64)->i64{return value}\n";CHECK(strlen(texts[mode])<sizeof(text)&&strlen(uri)<sizeof(root)&&strlen(lib)<sizeof(library));strcpy(text,texts[mode]);strcpy(root,uri);strcpy(library,mode==4?closed:lib);
    uint8_t *closed_bytes=NULL;size_t closed_length=0;XrXirStatus status=XR_XIR_OK;
    if(mode==4){char *path=NULL;status=xlsp_source_uri_path(r,closed,strlen(closed),&path);
        if(status==XR_XIR_OK){XrOsIoPolicy policy=xr_compile_io_policy(r);XrOsIoStatus read=xr_os_io_read_regular_file(&policy,path,UINT32_MAX,&closed_bytes,&closed_length);
            status=read==XR_OS_IO_OK?XR_XIR_OK:read==XR_OS_IO_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:read==XR_OS_IO_BUDGET?XR_XIR_BUDGET:XR_XIR_IO;}xr_compile_resources_free(path);}
    XlspSourceDocument documents[]={{root,text,strlen(root),strlen(text),7},{library,child,strlen(library),sizeof(child)-1,7}};
    if(mode==4){documents[1].text=(const char *)closed_bytes;documents[1].length=closed_length;documents[1].version=0;}
    if(mode==5)documents[0].version=INT32_MAX;bool known_versions[]={true,false};
    XrLspPosition position=mode==0||mode==6?(XrLspPosition){0,10}:mode==1?(XrLspPosition){0,5}:mode==2?(XrLspPosition){1,20}:mode==5?(XrLspPosition){0,17}:mode==7?(XrLspPosition){1,8}:(XrLspPosition){1,29};
    const char *name=mode==2?"callback":mode==5?"fresh\xf0\x9f\x98\x80":mode==6?"value":"changed";
    XlspSourceRenameInput input={0,position,name,strlen(name),mode==1||mode==7,mode==4?known_versions:NULL};
    lsp_json_calls=0;lsp_json_fail=json_fault;lsp_json_injected=false;
    if(status==XR_XIR_OK)status=xlsp_source_rename_json(&context,documents,(mode==3||mode==4)?2:1,&input,&result);
    if(closed_bytes)memset(closed_bytes,'?',closed_length);xr_compile_resources_free(closed_bytes);
    if(injected)*injected=lsp_json_injected;lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
    if(stats){*stats=sdk_stats(r);if(status!=XR_XIR_OK)fprintf(stderr,"Rename diagnostic mode=%u status=%u\n",mode,(unsigned)status);}
    memset(text,'?',sizeof(text));memset(root,'?',sizeof(root));memset(library,'?',sizeof(library));memset(child,'?',sizeof(child));memset(documents,0,sizeof(documents));
    if(status==XR_XIR_OK){facts(result,uri,mode);XrJsonValue *occupied=result;size_t before=runtime_attempts,json_before=lsp_json_calls;
        CHECK(xlsp_source_rename_json(&context,documents,1,&input,&occupied)==XR_XIR_BAD_STRUCTURE&&occupied==result&&before==runtime_attempts&&json_before==lsp_json_calls);}
    else CHECK(!result);xr_compile_resources_release(r);CHECK(!runtime_live&&!runtime_bytes);
    if(status==XR_XIR_OK)facts(result,uri,mode);xjson_free(result);CHECK(!lsp_json_live&&!lsp_json_bytes);return status;
}
static void rejected(const char *uri,const char *library) {
    const char *const programs[]={
        "fn run(x:i64)->i64{var y=2;return x+y}",
        "fn run(value:i64)->i64{return value}",
        "fn run(value:i64)->i64{return value}",
        "fn run(value:i64)->i64{return value}",
        "/* \xf0\x9f\x98\x80 */ fn run(value:i64)->i64{return value}",
        "import {target as alias} from \"./lib\"\nfn run()->i64{return alias(1)}",
        "import {target} from \"./lib\"\nfn run()->i64{return target(1)}",
        "var counter=0",
        "fn run(x:i64)->i64{\nvar result=0\n{var y=2;result=x+y}\nreturn result\n}"
    };
    const char *const names[]={"y","return","\x80","a\0b","changed","changed","changed","changed","x"};
    const size_t lengths[]={1,6,1,3,7,7,7,7,1};
    const XrLspPosition positions[]={{0,7},{0,8},{0,8},{0,8},{0,4},{1,22},{0,11},{0,0},{2,5}};
    const XrXirStatus expected[]={XR_XIR_BAD_TYPE,XR_XIR_BAD_STRUCTURE,XR_XIR_BAD_STRUCTURE,XR_XIR_BAD_STRUCTURE,
        XR_XIR_BAD_STRUCTURE,XR_XIR_UNSUPPORTED,XR_XIR_BAD_STRUCTURE,XR_XIR_UNRESOLVED,XR_XIR_BAD_TYPE};
    const char child[]="export fn target(value:i64)->i64{return value}";
    for(unsigned i=0;i<sizeof(programs)/sizeof(*programs);++i) {
        XrCompileResources *r=sdk_ledger(&limits);XrXirCompileContext context={r,xr_xir_compile_default_limits()};
        XlspSourceDocument documents[]={{uri,programs[i],strlen(uri),strlen(programs[i]),7},{library,child,strlen(library),sizeof(child)-1,9}};
        size_t count=i==5||i==6?2:1;XlspSourceBindings *baseline=NULL;
        CHECK(xlsp_source_bindings_build(&context,documents,count,&baseline)==XR_XIR_OK&&baseline);xlsp_source_bindings_free(baseline);
        XlspSourceRenameInput input={i==6?1:0,positions[i],names[i],lengths[i],i==7,NULL};XrJsonValue *result=NULL;
        XrXirStatus status=xlsp_source_rename_json(&context,documents,count,&input,&result);
        if(status!=expected[i])fprintf(stderr,"Rename rejection case=%u actual=%u expected=%u\n",i,(unsigned)status,(unsigned)expected[i]);
        CHECK(status==expected[i]&&!result);xr_compile_resources_release(r);CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes);
    }
    printf("Source rename9 baseline-checked rejections; capture collision/keyword/UTF8/NUL/surrogate/import facts physical0/0\n");
}
static int enum_required(const char *uri) {
    const char *text="enum Result {\n"
        "    Ok { value: i64, code: i64 },\n"
        "    Err { value: string }\n"
        "}\n"
        "var made = Result.Ok { code: 7, value: 42 }\n"
        "var selected = match (made) {\n"
        "    Result.Ok { value, code: _ } -> value\n"
        "    Result.Err { value: message } -> 0\n"
        "}\n";
    XrCompileResources *r=sdk_ledger(&limits);XrXirCompileContext context={r,xr_xir_compile_default_limits()};
    XlspSourceDocument document={uri,text,strlen(uri),strlen(text),1};XlspSourceBindings *baseline=NULL;
    CHECK(xlsp_source_bindings_build(&context,&document,1,&baseline)==XR_XIR_OK&&baseline);
    const XrXirSourceView *view=xlsp_source_bindings_view(baseline,0);const XrXirSourceSyntaxView *syntax=xlsp_source_bindings_syntax(baseline,0);
    for(uint32_t i=0;i<view->declaration_count;++i)if(syntax->declarations[i].role==XR_XIR_SOURCE_SYNTAX_UNRESOLVED)
        fprintf(stderr,"Required enum Source declaration=%u kind=%u name=%s parent=%u role=%u nameSpan=%d:%d..%d:%d\n",
            i+1,(unsigned)view->declarations[i].kind,view->declarations[i].name,view->declarations[i].parent,(unsigned)syntax->declarations[i].role,
            syntax->declarations[i].name.line,syntax->declarations[i].name.column,syntax->declarations[i].name.end_line,syntax->declarations[i].name.end_column);
    for(uint32_t i=0;i<view->reference_count;++i){const XrXirSourceReference *reference=&view->references[i];
        const XrXirSourceDeclaration *binding=&view->declarations[reference->declaration-1],*target=&view->declarations[reference->target-1];
        fprintf(stderr,"Required enum ref=%u binding=%u:%u:%s target=%u:%u:%s access=%u span=%d:%d..%d:%d\n",i,
            reference->declaration,(unsigned)binding->kind,binding->name,reference->target,(unsigned)target->kind,target->name,(unsigned)reference->access,
            syntax->references[i].line,syntax->references[i].column,syntax->references[i].end_line,syntax->references[i].end_column);}
    xlsp_source_bindings_free(baseline);XlspSourceRenameInput input={0,{4,33},"payload",7,false,NULL};XrJsonValue *result=NULL;
    XrXirStatus status=xlsp_source_rename_json(&context,&document,1,&input,&result);xr_compile_resources_release(r);CHECK(!runtime_live&&!runtime_bytes);
    if(status!=XR_XIR_OK){fprintf(stderr,"Required original enum field rename status=%u, output=%s, compiler physical0/0; original valid3-edit obligation remains\n",(unsigned)status,result?"PARTIAL":"NULL");xjson_free(result);CHECK(false);}
    CHECK(result);XrJsonValue *documents=xjson_get_array(result,"documentChanges");CHECK(documents&&documents->as.array.count==1);
    XrJsonValue *edits=xjson_get_array(xjson_array_get(documents,0),"edits");CHECK(edits&&edits->as.array.count==3);
    xjson_free(result);CHECK(!lsp_json_live&&!lsp_json_bytes);return 0;
}
static int run(int argc,char **argv) {
    (void)sdk_fixture_malloc;(void)sdk_fixture_free;(void)sdk_ledger;(void)sdk_unlimited;
    CHECK(argc==4||argc==5);if(argc==5&&!strcmp(argv[4],"--enum-required"))return enum_required(argv[1]);
    bool full=argc==4;CHECK(full||!strcmp(argv[4],"--normal"));
    for(unsigned mode=0;mode<8;++mode){XrCompileResourceStats stats={0};runtime_attempts=0;runtime_fail_at=SIZE_MAX;CHECK(once(argv[1],argv[2],argv[3],mode,limits,&stats,SIZE_MAX,NULL)==XR_XIR_OK);size_t sites=runtime_attempts,json_sites=lsp_json_calls;
        if(full){for(size_t i=0;i<sites;++i){runtime_attempts=0;runtime_fail_at=i;CHECK(once(argv[1],argv[2],argv[3],mode,limits,NULL,SIZE_MAX,NULL)==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>i);}runtime_fail_at=SIZE_MAX;
            for(size_t i=0;i<json_sites;++i){bool injected=false;CHECK(once(argv[1],argv[2],argv[3],mode,limits,NULL,i,&injected)==XR_XIR_OUT_OF_MEMORY&&injected);}
            for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){XrCompileResourceLimits exact={stats.allocated_bytes,stats.peak_bytes,stats.work};uint64_t *bound=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;CHECK(*bound&&*bound<INT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);CHECK(once(argv[1],argv[2],argv[3],mode,exact,NULL,SIZE_MAX,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));}}
        printf("Source rename mode=%u SourceFI=%zu JSONFI=%zu full=%u direct binding/versioned edit/candidate check physical0/0\n",mode,sites,json_sites,full?1u:0u);
    }rejected(argv[1],argv[2]);return 0;
}
#if XR_OS_WINDOWS
int wmain(int argc,wchar_t **wide){XrWinPathStatus status;char **argv=xr_win_utf16_arguments(argc,wide,&status);CHECK(argv);int result=run(argc,argv);xr_win_utf8_arguments_free(argc,argv);return result;}
#else
int main(int argc,char **argv){return run(argc,argv);}
#endif
