/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * import_resources.h - Complete import rename failure prefixes and resource axes
 *
 * KEY CONCEPT:
 *   Every injected failure returns no JSON and releases both allocation domains.
 */
#ifndef LSP_IMPORT_RESOURCES_H
#define LSP_IMPORT_RESOURCES_H
typedef struct ImportResourceCase {const char *uri,*library;unsigned mode;} ImportResourceCase;
static XrXirStatus import_once(const ImportResourceCase *item,XrCompileResourceLimits caps,
    XrCompileResourceStats *stats,size_t json_fault,bool *injected) {
    XrCompileResources *r=NULL;XrCompileResourceStatus made=xr_compile_resources_new(&caps,&r);
    if(made!=XR_COMPILE_RESOURCE_OK)return made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    char uri[1024],library[1024],text[512],child[sizeof(import_child)];
    CHECK(strlen(item->uri)<sizeof(uri)&&strlen(item->library)<sizeof(library)&&strlen(import_program(item->mode))<sizeof(text));
    strcpy(uri,item->uri);strcpy(library,item->library);strcpy(text,import_program(item->mode));memcpy(child,import_child,sizeof(child));
    XrXirCompileContext context={r,xr_xir_compile_default_limits()};
    XlspSourceDocument documents[]={{uri,text,strlen(uri),strlen(text),7},{library,child,strlen(library),sizeof(child)-1,9}};
    const char *name=item->mode==5?"changed\xf0\x9f\x98\x80":"changed";
    XlspSourceRenameInput input={item->mode==3?1u:0u,item->mode==3?(XrLspPosition){0,12}:(XrLspPosition){1,22},name,strlen(name),false,NULL};
    lsp_json_calls=0;lsp_json_fail=json_fault;lsp_json_injected=false;XrJsonValue *result=NULL;
    XrXirStatus status=xlsp_source_rename_json(&context,documents,2,&input,&result);
    if(injected)*injected=lsp_json_injected;lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
    if(stats)*stats=sdk_stats(r);
    memset(uri,'?',sizeof(uri));memset(library,'?',sizeof(library));memset(text,'?',sizeof(text));memset(child,'?',sizeof(child));memset(documents,0,sizeof(documents));
    xr_compile_resources_release(r);CHECK(!runtime_live&&!runtime_bytes);
    if(status==XR_XIR_OK){CHECK(result);import_result_fact(result,item->uri,item->library,item->mode,name);}
    else CHECK(!result);
    xjson_free(result);CHECK(!lsp_json_live&&!lsp_json_bytes);return status;
}
static void import_resource_baseline(const ImportResourceCase *item) {
    runtime_attempts=0;runtime_fail_at=SIZE_MAX;XrCompileResourceStats stats={0};
    CHECK(import_once(item,limits,&stats,SIZE_MAX,NULL)==XR_XIR_OK);
    CHECK(runtime_attempts&&lsp_json_calls);
    printf("{\"phase\":\"baseline\",\"mode\":%u,\"source\":%zu,\"json\":%zu,\"allocated\":%llu,\"peak\":%llu,\"work\":%llu,\"physical_live\":%zu,\"physical_bytes\":%zu,\"json_live\":%zu,\"json_bytes\":%zu}\n",
        item->mode,runtime_attempts,lsp_json_calls,(unsigned long long)stats.allocated_bytes,
        (unsigned long long)stats.peak_bytes,(unsigned long long)stats.work,runtime_live,runtime_bytes,lsp_json_live,lsp_json_bytes);
}
static void import_resource_sweep(const ImportResourceCase *item,bool source,size_t begin,size_t count,size_t total) {
    CHECK(total&&count&&begin<total&&count<=total-begin);
    for(size_t i=begin;i<begin+count;++i) {
        runtime_attempts=0;runtime_fail_at=source?i:SIZE_MAX;bool injected=false;
        CHECK(import_once(item,limits,NULL,source?SIZE_MAX:i,&injected)==XR_XIR_OUT_OF_MEMORY);
        if(source)CHECK(runtime_attempts>i);else CHECK(injected);
    }
    runtime_fail_at=SIZE_MAX;
    printf("{\"phase\":\"sweep\",\"mode\":%u,\"domain\":\"%s\",\"start\":%zu,\"count\":%zu,\"total\":%zu,\"checked\":%zu,\"physical_live\":%zu,\"physical_bytes\":%zu,\"json_live\":%zu,\"json_bytes\":%zu}\n",
        item->mode,source?"source":"json",begin,count,total,count,runtime_live,runtime_bytes,lsp_json_live,lsp_json_bytes);
}
static void import_resource_axes(const ImportResourceCase *item,XrCompileResourceLimits baseline) {
    CHECK(baseline.allocated_bytes<=limits.allocated_bytes&&baseline.live_bytes<=limits.live_bytes&&baseline.work<=limits.work);
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits exact=baseline;uint64_t *bound=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;
        CHECK(*bound&&*bound<INT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);runtime_attempts=0;runtime_fail_at=SIZE_MAX;
        CHECK(import_once(item,exact,NULL,SIZE_MAX,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));
    }
    printf("{\"phase\":\"axes\",\"mode\":%u,\"checked\":9,\"physical_live\":%zu,\"physical_bytes\":%zu,\"json_live\":%zu,\"json_bytes\":%zu}\n",
        item->mode,runtime_live,runtime_bytes,lsp_json_live,lsp_json_bytes);
}
static int import_resource_command(int argc,char **argv) {
    if(argc<5)return -1;
    if(!strcmp(argv[4],"--import-serial")) {
        CHECK(argc==5);for(unsigned mode=0;mode<6;++mode)import_resource_baseline(&(ImportResourceCase){argv[1],argv[2],mode});return 0;
    }
    bool baseline=!strcmp(argv[4],"--import-baseline"),sweep=!strcmp(argv[4],"--import-sweep"),axes=!strcmp(argv[4],"--import-axes");
    if(!baseline&&!sweep&&!axes)return -1;
    CHECK(argc==(baseline?6:sweep?10:9));size_t mode=rename_resource_size(argv[sweep?6:5]);CHECK(mode<6);
    ImportResourceCase item={argv[1],argv[2],(unsigned)mode};
    if(baseline)import_resource_baseline(&item);
    else if(sweep){bool source=!strcmp(argv[5],"source");CHECK(source||!strcmp(argv[5],"json"));
        import_resource_sweep(&item,source,rename_resource_size(argv[7]),rename_resource_size(argv[8]),rename_resource_size(argv[9]));}
    else import_resource_axes(&item,(XrCompileResourceLimits){rename_resource_number(argv[6]),rename_resource_number(argv[7]),rename_resource_number(argv[8])});
    return 0;
}
#endif // LSP_IMPORT_RESOURCES_H
