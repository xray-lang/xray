/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XR_TEST_RENAME_RESOURCES_H
#define XR_TEST_RENAME_RESOURCES_H
typedef struct RenameResourceCase {const char *uri,*library,*closed;unsigned mode;} RenameResourceCase;
static uint64_t rename_resource_number(const char *text) {
    CHECK(text&&*text);uint64_t value=0;
    for(const char *p=text;*p;++p){CHECK(*p>='0'&&*p<='9');uint64_t digit=(uint64_t)(*p-'0');CHECK(value<=(UINT64_MAX-digit)/10);value=value*10+digit;}
    return value;
}
static size_t rename_resource_size(const char *text) {
    uint64_t value=rename_resource_number(text);
    size_t converted=(size_t)value;CHECK((uint64_t)converted==value);return converted;
}
static void rename_resource_balanced(void) {
    CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes);
}
static void rename_resource_baseline(const RenameResourceCase *item) {
    runtime_attempts=0;runtime_fail_at=SIZE_MAX;XrCompileResourceStats stats={0};
    CHECK(once(item->uri,item->library,item->closed,item->mode,limits,&stats,SIZE_MAX,NULL)==XR_XIR_OK);
    rename_resource_balanced();CHECK(runtime_attempts&&lsp_json_calls);
    printf("{\"phase\":\"baseline\",\"mode\":%u,\"source\":%zu,\"json\":%zu,\"allocated\":%llu,\"peak\":%llu,\"work\":%llu,\"physical_live\":%zu,\"physical_bytes\":%zu,\"json_live\":%zu,\"json_bytes\":%zu}\n",
        item->mode,runtime_attempts,lsp_json_calls,(unsigned long long)stats.allocated_bytes,
        (unsigned long long)stats.peak_bytes,(unsigned long long)stats.work,runtime_live,runtime_bytes,lsp_json_live,lsp_json_bytes);
}
static void rename_resource_sweep(const RenameResourceCase *item,bool source,size_t begin,size_t count,size_t total) {
    CHECK(total&&count&&begin<total&&count<=total-begin);
    for(size_t i=begin;i<begin+count;++i){
        runtime_attempts=0;runtime_fail_at=source?i:SIZE_MAX;bool injected=false;
        CHECK(once(item->uri,item->library,item->closed,item->mode,limits,NULL,source?SIZE_MAX:i,&injected)==XR_XIR_OUT_OF_MEMORY);
        if(source)CHECK(runtime_attempts>i);else CHECK(injected);
        rename_resource_balanced();
    }
    runtime_fail_at=SIZE_MAX;
    printf("{\"phase\":\"sweep\",\"mode\":%u,\"domain\":\"%s\",\"start\":%zu,\"count\":%zu,\"total\":%zu,\"checked\":%zu,\"physical_live\":%zu,\"physical_bytes\":%zu,\"json_live\":%zu,\"json_bytes\":%zu}\n",
        item->mode,source?"source":"json",begin,count,total,count,runtime_live,runtime_bytes,lsp_json_live,lsp_json_bytes);
}
static void rename_resource_axes(const RenameResourceCase *item,XrCompileResourceLimits baseline) {
    CHECK(baseline.allocated_bytes<=limits.allocated_bytes&&baseline.live_bytes<=limits.live_bytes&&baseline.work<=limits.work);
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){
        XrCompileResourceLimits exact=baseline;uint64_t *bound=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;
        CHECK(*bound&&*bound<INT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);runtime_attempts=0;runtime_fail_at=SIZE_MAX;
        CHECK(once(item->uri,item->library,item->closed,item->mode,exact,NULL,SIZE_MAX,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));
        rename_resource_balanced();
    }
    printf("{\"phase\":\"axes\",\"mode\":%u,\"checked\":9,\"physical_live\":%zu,\"physical_bytes\":%zu,\"json_live\":%zu,\"json_bytes\":%zu}\n",item->mode,runtime_live,runtime_bytes,lsp_json_live,lsp_json_bytes);
}
static int rename_resource_command(int argc,char **argv) {
    if(argc<5)return -1;
    if(!strcmp(argv[4],"--serial-baselines")){
        CHECK(argc==5);
        for(unsigned mode=0;mode<8;++mode)rename_resource_baseline(&(RenameResourceCase){argv[1],argv[2],argv[3],mode});
        rejected(argv[1],argv[2]);rename_resource_balanced();
        printf("{\"phase\":\"serial\",\"checked\":8,\"rejections\":9,\"physical_live\":%zu,\"physical_bytes\":%zu,\"json_live\":%zu,\"json_bytes\":%zu}\n",runtime_live,runtime_bytes,lsp_json_live,lsp_json_bytes);return 0;
    }
    if(!strcmp(argv[4],"--rejections")){
        CHECK(argc==5);rejected(argv[1],argv[2]);rename_resource_balanced();
        printf("{\"phase\":\"rejections\",\"checked\":9,\"physical_live\":%zu,\"physical_bytes\":%zu,\"json_live\":%zu,\"json_bytes\":%zu}\n",runtime_live,runtime_bytes,lsp_json_live,lsp_json_bytes);return 0;
    }
    bool baseline=!strcmp(argv[4],"--baseline"),sweep=!strcmp(argv[4],"--sweep"),axes=!strcmp(argv[4],"--axes");
    if(!baseline&&!sweep&&!axes)return -1;
    CHECK(argc==(baseline?6:sweep?10:9));size_t mode=rename_resource_size(argv[sweep?6:5]);CHECK(mode<8);
    RenameResourceCase item={argv[1],argv[2],argv[3],(unsigned)mode};
    if(baseline)rename_resource_baseline(&item);
    else if(sweep){bool source=!strcmp(argv[5],"source");CHECK(source||!strcmp(argv[5],"json"));
        rename_resource_sweep(&item,source,rename_resource_size(argv[7]),rename_resource_size(argv[8]),rename_resource_size(argv[9]));}
    else rename_resource_axes(&item,(XrCompileResourceLimits){rename_resource_number(argv[6]),rename_resource_number(argv[7]),rename_resource_number(argv[8])});
    return 0;
}
#endif
