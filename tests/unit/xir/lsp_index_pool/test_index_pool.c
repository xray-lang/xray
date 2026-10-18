/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_index_pool.h"
#include "app/lsp/xlsp_index_scan.h"
#include "os/os_thread.h"
#include "os/os_fs.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
typedef union PoolBlock {long double alignment;void *pointer_alignment;size_t bytes;} PoolBlock;
static _Atomic size_t physical_calls,physical_live,physical_bytes;
static size_t physical_fail=SIZE_MAX;
static void *physical_malloc(size_t bytes) {
    if(atomic_fetch_add(&physical_calls,1)==physical_fail)return NULL;
    CHECK(bytes<=SIZE_MAX-sizeof(PoolBlock));PoolBlock *block=xr_malloc(sizeof(*block)+bytes);if(!block)return NULL;
    block->bytes=bytes;atomic_fetch_add(&physical_live,1);atomic_fetch_add(&physical_bytes,bytes);return block+1;
}
static void physical_free(void *pointer) {
    if(!pointer)return;PoolBlock *block=(PoolBlock *)pointer-1;
    CHECK(atomic_load(&physical_live));atomic_fetch_sub(&physical_live,1);atomic_fetch_sub(&physical_bytes,block->bytes);xr_free(block);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(n) physical_malloc(n)
#define xr_free(p) physical_free(p)
#include "base/xcompile_resources.c"
#include "os/win/thread_win.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")
static _Atomic bool release_reads;
static _Atomic unsigned read_arrivals;
static XrOsIoStatus real_read_barrier(const XrOsIoPolicy *policy,const char *path,size_t limit,uint8_t **out,size_t *size) {
    atomic_fetch_add(&read_arrivals,1);while(!atomic_load(&release_reads))xr_thread_yield();
    return xr_os_io_read_regular_file(policy,path,limit,out,size);
}
#define xr_os_io_read_regular_file real_read_barrier
#include "app/lsp/xlsp_index_pool.c"
#undef xr_os_io_read_regular_file
_Static_assert(_Alignof(PoolBlock)>=_Alignof(XrCompileResources),"Physical prefix must preserve ledger alignment");
_Static_assert(_Alignof(PoolBlock)>=_Alignof(XrLspIndexPool),"Physical prefix must preserve pool alignment");
static bool ignore_file(const char *name,bool directory,void *context) {
    (void)directory;(void)context;return !strcmp(name,".ignored");
}
static const XrCompileResourceLimits default_limits={UINT64_C(1073741824),UINT64_C(268435456),UINT64_C(8589934592)};
static void result_facts(const XrLspIndexResult *list) {
    unsigned flags=0;size_t total=0;
    for(const XrLspIndexResult *n=list;n;n=n->next){CHECK(n->status==XR_XIR_OK&&!atomic_load(&n->cancel_requested)&&n->index&&n->revision==71);
        CHECK(strstr(n->uri,"fixture-inputs%20%23")&&xlsp_source_index_version(n->index)==0&&!strcmp(n->uri,xlsp_source_index_uri(n->index)));
        size_t count=0;const XlspSourceIndexSymbol *items=xlsp_source_index_symbols(n->index,&count);CHECK(count==1&&items);++total;
        if(!strcmp(items[0].name,"alpha")){flags|=1;CHECK(items[0].exported&&items[0].selection.start.character==19&&items[0].selection.end.character==24);}
        else if(!strcmp(items[0].name,"kept")){flags|=2;CHECK(xlsp_source_index_parse_status(n->index)==XR_PARSE_RECOVERED&&items[0].selection.start.character==10);}
        else {CHECK(!strcmp(items[0].name,"third"));flags|=4;CHECK(strstr(n->uri,"%F0%9F%98%80")&&items[0].selection.start.character==3);}}
    CHECK(total==3&&flags==7);
}
static XrXirStatus trace(const char *root,XrCompileResourceLimits limits,unsigned workers,unsigned cancel,XrCompileResourceStats *stats) {
    XrCompileResources *resources=NULL;XrLspIndexPool *pool=NULL;XrLspIndexResult *results=NULL,*tail=NULL;XlspIndexPaths paths={0};bool external=true;char cancel_path[1024]={0};
    XrCompileResourceStatus made=xr_compile_resources_new(&limits,&resources);
    if(made!=XR_COMPILE_RESOURCE_OK)return made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    const char *roots[]={root};XlspIndexScanOptions scan={resources,roots,1,ignore_file,NULL};
    XrXirStatus status=xlsp_index_scan(&scan,&paths);if(status!=XR_XIR_OK)goto done;CHECK(paths.count==3);
    XlspIndexPoolOptions options={resources,1024,workers};status=xlsp_index_pool_new(&options,&pool);if(status!=XR_XIR_OK)goto done;
    atomic_store(&release_reads,false);atomic_store(&read_arrivals,0);
    status=xlsp_index_pool_submit_batch(pool,paths.values,paths.count,71);if(status!=XR_XIR_OK)goto done;
    if(workers==2){unsigned wait=0;while(atomic_load(&read_arrivals)<2&&wait++<5000)xr_thread_sleep_ms(1);CHECK(atomic_load(&read_arrivals)==2);}
    if(cancel==1)xlsp_index_pool_cancel_all(pool);
    if(cancel>=2){bool found=false;const char *target=cancel==3?"emoji-":"alpha.xr";
        for(size_t i=0;i<paths.count;++i)if(strstr(paths.values[i],target)){size_t width=strlen(paths.values[i]);CHECK(width<sizeof(cancel_path));memcpy(cancel_path,paths.values[i],width+1);found=true;}CHECK(found);
        if(cancel!=4)xlsp_index_pool_cancel_file(pool,cancel_path);}
    xlsp_index_paths_free(&paths);xr_compile_resources_release(resources);external=false;atomic_store(&release_reads,true);
    for(unsigned wait=0;wait<20000;++wait){XrLspIndexResult *batch=cancel==4?NULL:xlsp_index_pool_poll(pool);
        if(batch){if(tail)tail->next=batch;else results=batch;while(batch->next)batch=batch->next;tail=batch;}
        uint64_t submitted=0,completed=0;xlsp_index_pool_get_progress(pool,&submitted,&completed);CHECK(submitted==3&&completed<=3);
        if(completed==3&&xlsp_index_pool_is_idle(pool)){
            if(cancel==4)xlsp_index_pool_cancel_file(pool,cancel_path);
            batch=xlsp_index_pool_poll(pool);if(batch){if(tail)tail->next=batch;else results=batch;while(batch->next)batch=batch->next;tail=batch;}break;}
        xr_thread_sleep_ms(1);if(wait==19999)CHECK(false);}
    {size_t count=0,cancelled=0;unsigned healthy=0;for(XrLspIndexResult *n=results;n;n=n->next){++count;
        if(cancel==1)CHECK(atomic_load(&n->cancel_requested)&&!n->index);
        else if(cancel>=2&&atomic_load(&n->cancel_requested)){CHECK((cancel==4?n->index!=NULL:n->index==NULL)&&strstr(n->path,cancel==3?"emoji-":"alpha.xr"));++cancelled;}
        else if(n->status!=XR_XIR_OK){CHECK(n->status==XR_XIR_OUT_OF_MEMORY||n->status==XR_XIR_BUDGET);status=n->status;}
        else if(cancel>=2){size_t total=0;const XlspSourceIndexSymbol *items=xlsp_source_index_symbols(n->index,&total);CHECK(total==1);if(!strcmp(items[0].name,"alpha"))healthy|=1;else if(!strcmp(items[0].name,"kept"))healthy|=2;else{CHECK(!strcmp(items[0].name,"third"));healthy|=4;}}}
     CHECK(count==3);if(cancel>=2)CHECK(cancelled==1&&healthy==(cancel==3?3u:6u));}
done:
    atomic_store(&release_reads,true);xlsp_index_paths_free(&paths);
    if(stats)CHECK(xr_compile_resources_stats(resources,stats)==XR_COMPILE_RESOURCE_OK);
    xlsp_index_pool_free(pool);if(external)xr_compile_resources_release(resources);
    if(status==XR_XIR_OK&&!cancel)result_facts(results);
    xlsp_index_result_free_list(results);CHECK(!atomic_load(&physical_live)&&!atomic_load(&physical_bytes));return status;
}
static void atomic_admission(const char *root) {
    XrCompileResources *resources=NULL;CHECK(xr_compile_resources_new(&default_limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XlspIndexPaths paths={0};const char *roots[]={root};XlspIndexScanOptions scan={resources,roots,1,ignore_file,NULL};CHECK(xlsp_index_scan(&scan,&paths)==XR_XIR_OK);
    XrLspIndexPool *pool=NULL;XlspIndexPoolOptions options={resources,1024,2};CHECK(xlsp_index_pool_new(&options,&pool)==XR_XIR_OK);
    char *invalid[]={paths.values[0],NULL,paths.values[2]};CHECK(xlsp_index_pool_submit_batch(pool,invalid,3,71)==XR_XIR_BAD_STRUCTURE);
    CHECK(xlsp_index_pool_submit(pool,paths.values[0],"file:///E:/different.xr",71)==XR_XIR_BAD_STRUCTURE);
    uint64_t submitted=99,completed=99;xlsp_index_pool_get_progress(pool,&submitted,&completed);CHECK(!submitted&&!completed&&xlsp_index_pool_is_idle(pool)&&!xlsp_index_pool_poll(pool));
    XrLspIndexPool *occupied=pool;size_t before=atomic_load(&physical_calls);CHECK(xlsp_index_pool_new(&options,&occupied)==XR_XIR_BAD_STRUCTURE&&occupied==pool&&before==atomic_load(&physical_calls));
    atomic_store(&release_reads,true);CHECK(xlsp_index_pool_submit_batch(pool,paths.values,3,71)==XR_XIR_OK);
    xlsp_index_pool_free(pool);xlsp_index_paths_free(&paths);xr_compile_resources_release(resources);CHECK(!atomic_load(&physical_live)&&!atomic_load(&physical_bytes));
}
static void constructor_faults(unsigned workers,bool faults) {
    size_t sites=0;
    for(size_t probe=0;probe<=sites;++probe){atomic_store(&physical_calls,0);physical_fail=probe?probe-1:SIZE_MAX;
        XrCompileResources *resources=NULL;XrLspIndexPool *pool=NULL;XrXirStatus status=XR_XIR_OK;
        XrCompileResourceStatus made=xr_compile_resources_new(&default_limits,&resources);
        if(made!=XR_COMPILE_RESOURCE_OK)status=XR_XIR_OUT_OF_MEMORY;
        else {XlspIndexPoolOptions options={resources,1024,workers};status=xlsp_index_pool_new(&options,&pool);}
        if(!probe){CHECK(status==XR_XIR_OK&&pool);if(faults)sites=atomic_load(&physical_calls);}
        else CHECK((status==XR_XIR_OUT_OF_MEMORY||status==XR_XIR_IO)&&!pool&&atomic_load(&physical_calls)>=probe);
        xlsp_index_pool_free(pool);xr_compile_resources_release(resources);CHECK(!atomic_load(&physical_live)&&!atomic_load(&physical_bytes));
    }
    physical_fail=SIZE_MAX;printf("LSP index constructor workers=%u actual FI=%zu full=%d; every prefix joined, physical=0/0\n",workers,sites,faults?1:0);
}
int main(int argc,char **argv) {
    CHECK(argc==2||(argc==3&&!strcmp(argv[2],"--normal")));bool faults=argc==2;size_t sites=0;XrCompileResourceStats needed={0};
    atomic_store(&physical_calls,0);physical_fail=SIZE_MAX;CHECK(trace(argv[1],default_limits,1,false,&needed)==XR_XIR_OK);sites=atomic_load(&physical_calls);
    if(faults)for(size_t site=0;site<sites;++site){atomic_store(&physical_calls,0);physical_fail=site;XrXirStatus s=trace(argv[1],default_limits,1,false,NULL);
        CHECK((s==XR_XIR_OUT_OF_MEMORY||s==XR_XIR_IO)&&atomic_load(&physical_calls)>site);}
    physical_fail=SIZE_MAX;
    if(faults)for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){
        XrCompileResourceLimits exact={needed.allocated_bytes,needed.peak_bytes,needed.work};uint64_t *bound=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;
        CHECK(*bound&&*bound<INT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);CHECK(trace(argv[1],exact,1,false,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));}
    CHECK(trace(argv[1],default_limits,2,false,NULL)==XR_XIR_OK);CHECK(trace(argv[1],default_limits,2,1,NULL)==XR_XIR_OK);CHECK(trace(argv[1],default_limits,2,2,NULL)==XR_XIR_OK);CHECK(trace(argv[1],default_limits,2,3,NULL)==XR_XIR_OK);CHECK(trace(argv[1],default_limits,2,4,NULL)==XR_XIR_OK);atomic_admission(argv[1]);
    constructor_faults(1,faults);constructor_faults(2,faults);constructor_faults(64,faults);
    printf("LSP actual parallel index/scan Source+OS starter FI=%zu full=%d;2 real read entrants, cancellation, atomic batch, pool/Session/input death, physical=0/0\n",sites,faults?1:0);return 0;
}
