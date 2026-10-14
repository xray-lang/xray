/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_async.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
typedef union AsyncBlock {long double alignment;void *pointer_alignment;size_t bytes;} AsyncBlock;
_Static_assert(_Alignof(AsyncBlock)>=_Alignof(XrLspAsync),"Allocator prefix must preserve worker alignment");
_Static_assert(_Alignof(AsyncBlock)>=_Alignof(XrLspTask),"Allocator prefix must preserve task alignment");
static _Atomic size_t allocation_calls,allocation_live,allocation_bytes;
static size_t allocation_fail=SIZE_MAX;
static void *owner_malloc(size_t bytes) {
    size_t site=atomic_fetch_add(&allocation_calls,1);
    if(site==allocation_fail)return NULL;
    CHECK(bytes<=SIZE_MAX-sizeof(AsyncBlock));
    AsyncBlock *block=xr_malloc(sizeof(*block)+bytes);if(!block)return NULL;
    block->bytes=bytes;atomic_fetch_add(&allocation_live,1);atomic_fetch_add(&allocation_bytes,bytes);
    return block+1;
}
static void *owner_calloc(size_t count,size_t bytes) {
    CHECK(!count||bytes<=SIZE_MAX/count);void *out=owner_malloc(count*bytes);
    if(out)memset(out,0,count*bytes);return out;
}
static void owner_free(void *pointer) {
    if(!pointer)return;AsyncBlock *block=(AsyncBlock *)pointer-1;
    CHECK(atomic_load(&allocation_live));atomic_fetch_sub(&allocation_live,1);
    atomic_fetch_sub(&allocation_bytes,block->bytes);xr_free(block);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_calloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_calloc
#undef xr_free
#define xr_malloc(n) owner_malloc(n)
#define xr_calloc(n,s) owner_calloc(n,s)
#define xr_free(p) owner_free(p)
#include "app/lsp/xlsp_async.c"
#include "os/win/thread_win.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_calloc")
#pragma pop_macro("xr_malloc")
typedef struct OwnerTask {
    XrLspTask *task;
    xr_mutex_t mutex;
    xr_cond_t condition;
    _Atomic bool release;
    bool started,expect_null;
    unsigned executions,completions;
} OwnerTask;
static void task_init(OwnerTask *owner,bool expect_null) {
    memset(owner,0,sizeof(*owner));xr_mutex_init(&owner->mutex);xr_cond_init(&owner->condition);
    atomic_init(&owner->release,false);owner->expect_null=expect_null;
}
static void task_destroy(OwnerTask *owner) {
    xr_cond_destroy(&owner->condition);xr_mutex_destroy(&owner->mutex);
}
static void owner_execute(void *data) {
    OwnerTask *owner=data;
    CHECK(owner->task->data==owner&&atomic_load(&owner->task->state)==XLSP_TASK_RUNNING);
    xr_mutex_lock(&owner->mutex);++owner->executions;owner->started=true;
    xr_cond_signal(&owner->condition);xr_mutex_unlock(&owner->mutex);
    while(!atomic_load(&owner->release)&&!xlsp_task_is_cancelled(owner->task))xr_thread_yield();
    owner->task->result=owner->task;
}
static void owner_complete(void *data,void *result) {
    OwnerTask *owner=data;CHECK(owner->task->data==owner&&owner->task->task_id);
    CHECK(owner->completions==0);++owner->completions;
    CHECK(owner->expect_null?result==NULL:result==owner->task);
}
static bool owner_submit(XrLspAsync *async,OwnerTask *owner) {
    owner->task=xlsp_task_new(owner_execute,owner_complete,owner,XLSP_TASK_NORMAL);
    if(!owner->task)return false;
    CHECK(xlsp_async_submit(async,owner->task));return true;
}
static void owner_started(OwnerTask *owner) {
    xr_mutex_lock(&owner->mutex);
    while(!owner->started)CHECK(xr_cond_wait_for_ns(&owner->condition,&owner->mutex,UINT64_C(2000000000)));
    xr_mutex_unlock(&owner->mutex);
}
static void owner_poll(XrLspAsync *async,OwnerTask *first,OwnerTask *second) {
    for(unsigned i=0;i<2000&&(!first->completions||(second&&!second->completions));++i) {
        xlsp_async_poll(async);xr_thread_sleep_ms(1);
    }
    CHECK(first->completions==1&&(!second||second->completions==1));
    CHECK(atomic_load(&async->completed_count)==0&&atomic_load(&async->pending_count)==0);
    CHECK(xlsp_async_poll(async)==0);
}
static bool owner_cycle(bool shutdown) {
    XrLspAsync *async=xlsp_async_new(NULL,NULL);if(!async)return false;
    // Empty polls must return without a Windows CRT pipe read.
    CHECK(xlsp_async_poll(async)==0);
    OwnerTask first,second;task_init(&first,true);task_init(&second,true);
    bool admitted=owner_submit(async,&first);
    if(admitted) {
        owner_started(&first);admitted=owner_submit(async,&second);
        if(admitted&&!shutdown) {
            CHECK(xlsp_async_cancel_task(async,second.task->task_id));
            CHECK(xlsp_async_cancel_task(async,first.task->task_id));
            owner_poll(async,&first,&second);
            CHECK(first.executions==1&&second.executions==0&&atomic_load(&async->cancelled_tasks)==2);
        }
    }
    xlsp_async_free(async);
    CHECK(!first.task||first.completions==1);CHECK(!second.task||second.completions==1);
    task_destroy(&first);task_destroy(&second);
    CHECK(!atomic_load(&allocation_live)&&!atomic_load(&allocation_bytes));return admitted;
}
static void owner_normal(void) {
    XrLspAsync *async=xlsp_async_new(NULL,NULL);CHECK(async);
    OwnerTask first;task_init(&first,false);CHECK(owner_submit(async,&first));owner_started(&first);
    atomic_store(&first.release,true);owner_poll(async,&first,NULL);CHECK(first.executions==1);
    xlsp_async_free(async);task_destroy(&first);
    CHECK(!atomic_load(&allocation_live)&&!atomic_load(&allocation_bytes));
}
static void owner_id_boundary(void) {
    XrLspAsync *async=xlsp_async_new(NULL,NULL);CHECK(async);
    OwnerTask first;task_init(&first,false);
    first.task=xlsp_task_new(owner_execute,owner_complete,&first,XLSP_TASK_NORMAL);CHECK(first.task);
    atomic_store(&async->next_task_id,UINT64_MAX);CHECK(!xlsp_async_submit(async,first.task));
    CHECK(!first.task->task_id&&!atomic_load(&async->pending_count));
    atomic_store(&async->next_task_id,UINT64_MAX-1);
    CHECK(xlsp_async_submit(async,first.task)==UINT64_MAX-1);owner_started(&first);
    XrLspTask *extra=xlsp_task_new(owner_execute,owner_complete,&first,XLSP_TASK_NORMAL);CHECK(extra);
    CHECK(!xlsp_async_submit(async,extra)&&!extra->task_id);xlsp_task_free(extra);
    atomic_store(&first.release,true);owner_poll(async,&first,NULL);xlsp_async_free(async);task_destroy(&first);
    CHECK(!atomic_load(&allocation_live)&&!atomic_load(&allocation_bytes));
}
int main(void) {
    owner_normal();owner_id_boundary();CHECK(owner_cycle(false));
    atomic_store(&allocation_calls,0);CHECK(owner_cycle(true));size_t sites=atomic_load(&allocation_calls);CHECK(sites);
    for(size_t i=0;i<sites;++i) {
        atomic_store(&allocation_calls,0);allocation_fail=i;CHECK(!owner_cycle(true));
        CHECK(atomic_load(&allocation_calls)>=i+1&&!atomic_load(&allocation_live)&&!atomic_load(&allocation_bytes));
    }
    allocation_fail=SIZE_MAX;
    for(unsigned i=0;i<32;++i){CHECK(owner_cycle(false));CHECK(owner_cycle(true));}
    printf("LSP real worker: stable task identity, running/queued cancel, shutdown, ID exhaustion, FI=%zu, physical=0/0\n",sites);
    return 0;
}
