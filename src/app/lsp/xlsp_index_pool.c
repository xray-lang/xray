/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_index_pool.c - Same-ledger workers and explicit result ownership
 */
#include "xlsp_index_pool.h"
#include "../../os/os_thread.h"
#include "../../os/os_fs.h"
#include "../../base/xchecks.h"
#include "../../base/xutf8.h"
#include <string.h>
typedef struct IndexWorker {struct XrLspIndexPool *pool;xr_thread_t thread;XrLspIndexResult *current;} IndexWorker;
struct XrLspIndexPool {
    XrCompileResources *resources;
    size_t maximum_file_bytes,queued,active;
    uint64_t submitted,completed;
    unsigned worker_count;
    bool running;
    xr_mutex_t mutex;
    xr_cond_t condition;
    XrLspIndexResult *head,*tail,*results,*result_tail;
    IndexWorker workers[64];
};
static XrXirStatus pool_resource(XrCompileResourceStatus s) {
    return s==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:s==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        s==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus pool_work(XrCompileResources *r,size_t n){return pool_resource(xr_compile_resources_work(r,n));}
static XrXirStatus pool_length(XrCompileResources *r,const char *text,size_t *length) {
    if(!text)return XR_XIR_BAD_STRUCTURE;
    for(size_t i=0;;++i){XrXirStatus s=pool_work(r,1);if(s!=XR_XIR_OK)return s;
        if(!text[i]){*length=i;return XR_XIR_OK;}if(i==SIZE_MAX-1)return XR_XIR_BUDGET;}
}
static XrXirStatus pool_copy(XrCompileResources *r,const char *text,char **out) {
    size_t n=0;XrXirStatus s=pool_length(r,text,&n);if(s!=XR_XIR_OK)return s;
    s=pool_work(r,n+1);if(s!=XR_XIR_OK)return s;char *copy=NULL;
    s=pool_resource(xr_compile_resources_alloc(r,n+1,(void **)&copy));
    if(s==XR_XIR_OK){memcpy(copy,text,n+1);*out=copy;}return s;
}

static unsigned char pool_separator(unsigned char c) {
#ifdef XR_OS_WINDOWS
    if(c=='\\')return '/';
#endif
    return c;
}


XR_FUNC void xlsp_index_result_free(XrLspIndexResult *n) {
    if(!n)return;xlsp_source_index_free(n->index);xr_compile_resources_free(n->path);xr_compile_resources_free(n->uri);xr_compile_resources_free(n);
}
XR_FUNC void xlsp_index_result_free_list(XrLspIndexResult *n){while(n){XrLspIndexResult *next=n->next;xlsp_index_result_free(n);n=next;}}
static XrXirStatus pool_capture(XrLspIndexPool *p,const char *path,const char *uri,uint64_t revision,XrLspIndexResult **out) {
    XrLspIndexResult *n=NULL;XrXirStatus s=pool_resource(xr_compile_resources_calloc(p->resources,1,sizeof(*n),(void **)&n));
    if(s!=XR_XIR_OK)return s;atomic_init(&n->cancel_requested,false);n->revision=revision;
    s=pool_copy(p->resources,path,&n->path);
    if(s==XR_XIR_OK)s=uri?pool_copy(p->resources,uri,&n->uri):xlsp_index_file_uri(p->resources,path,&n->uri);
    char *decoded=NULL;size_t length=0;
    if(s==XR_XIR_OK)s=pool_length(p->resources,n->uri,&length);
    if(s==XR_XIR_OK)s=xlsp_source_uri_path(p->resources,n->uri,length,&decoded);
    if(s==XR_XIR_OK)for(size_t i=0;;++i){s=pool_work(p->resources,2);if(s!=XR_XIR_OK)break;
        unsigned char a=pool_separator((unsigned char)decoded[i]),b=pool_separator((unsigned char)n->path[i]);
        if(a!=b){s=XR_XIR_BAD_STRUCTURE;break;}if(!a)break;}
    xr_compile_resources_free(decoded);if(s==XR_XIR_OK)*out=n;else xlsp_index_result_free(n);return s;
}
static void pool_parse(XrLspIndexPool *p,XrLspIndexResult *n) {
    if(atomic_load(&n->cancel_requested))return;
    XrOsIoPolicy policy=xr_compile_io_policy(p->resources);uint8_t *text=NULL;size_t length=0;
    XrOsIoStatus read=xr_os_io_read_regular_file(&policy,n->path,p->maximum_file_bytes,&text,&length);
    n->status=read==XR_OS_IO_OK?XR_XIR_OK:read==XR_OS_IO_BUDGET?XR_XIR_BUDGET:
        read==XR_OS_IO_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:read==XR_OS_IO_BAD_ARGUMENT?XR_XIR_BAD_STRUCTURE:XR_XIR_IO;
    if(n->status==XR_XIR_OK&&!atomic_load(&n->cancel_requested)){
        size_t uri_length=0;n->status=pool_length(p->resources,n->uri,&uri_length);
        XlspSourceDocument input={n->uri,(const char *)text,uri_length,length,0};
        if(n->status==XR_XIR_OK)n->status=xlsp_source_index_build(p->resources,&input,&n->index);}
    xr_compile_resources_free(text);if(atomic_load(&n->cancel_requested)){xlsp_source_index_free(n->index);n->index=NULL;}
}
static void *pool_worker(void *context) {
    IndexWorker *w=context;XrLspIndexPool *p=w->pool;
    for(;;){xr_mutex_lock(&p->mutex);while(p->running&&!p->head)xr_cond_wait(&p->condition,&p->mutex);
        if(!p->running){xr_mutex_unlock(&p->mutex);break;}
        XrLspIndexResult *n=p->head;p->head=n->next;if(!p->head)p->tail=NULL;n->next=NULL;
        --p->queued;++p->active;w->current=n;xr_mutex_unlock(&p->mutex);pool_parse(p,n);
        xr_mutex_lock(&p->mutex);w->current=NULL;--p->active;++p->completed;
        if(p->result_tail)p->result_tail->next=n;else p->results=n;p->result_tail=n;xr_mutex_unlock(&p->mutex);}
    return NULL;
}
static void pool_cancel_locked(XrLspIndexPool *p) {
    for(XrLspIndexResult *n=p->head;n;n=n->next)atomic_store(&n->cancel_requested,true);
    for(XrLspIndexResult *n=p->results;n;n=n->next)atomic_store(&n->cancel_requested,true);
    for(unsigned i=0;i<p->worker_count;++i)if(p->workers[i].current)atomic_store(&p->workers[i].current->cancel_requested,true);
}
XR_FUNC void xlsp_index_pool_cancel_all(XrLspIndexPool *p){if(!p)return;xr_mutex_lock(&p->mutex);pool_cancel_locked(p);xr_mutex_unlock(&p->mutex);}
static void pool_cancel_file(XrLspIndexResult *node,const char *path) {
    if(!node)return;
    for(size_t i=0;;++i){unsigned char a=pool_separator((unsigned char)node->path[i]),b=pool_separator((unsigned char)path[i]);
        if(a!=b)return;if(!a)break;}
    atomic_store(&node->cancel_requested,true);
}
XR_FUNC void xlsp_index_pool_cancel_file(XrLspIndexPool *p,const char *path) {
    if(!p||!path)return;xr_mutex_lock(&p->mutex);
    for(XrLspIndexResult *n=p->head;n;n=n->next)pool_cancel_file(n,path);
    for(XrLspIndexResult *n=p->results;n;n=n->next)pool_cancel_file(n,path);
    for(unsigned i=0;i<p->worker_count;++i)pool_cancel_file(p->workers[i].current,path);
    xr_mutex_unlock(&p->mutex);
}
XR_FUNC void xlsp_index_pool_free(XrLspIndexPool *p) {
    if(!p)return;xr_mutex_lock(&p->mutex);p->running=false;pool_cancel_locked(p);xr_cond_broadcast(&p->condition);xr_mutex_unlock(&p->mutex);
    for(unsigned i=0;i<p->worker_count;++i)XR_CHECK(xr_thread_join(p->workers[i].thread,NULL)==0,"Index worker must join before owner release");
    xlsp_index_result_free_list(p->head);xlsp_index_result_free_list(p->results);xr_cond_destroy(&p->condition);xr_mutex_destroy(&p->mutex);xr_compile_resources_free(p);
}
XR_FUNC XrXirStatus xlsp_index_pool_new(const XlspIndexPoolOptions *o,XrLspIndexPool **out) {
    if(!o||!o->resources||!o->workers||o->workers>64||!o->maximum_file_bytes||!out||*out)return XR_XIR_BAD_STRUCTURE;
    XrLspIndexPool *p=NULL;XrXirStatus s=pool_resource(xr_compile_resources_calloc(o->resources,1,sizeof(*p),(void **)&p));if(s!=XR_XIR_OK)return s;
    p->resources=o->resources;p->maximum_file_bytes=o->maximum_file_bytes;p->running=true;xr_mutex_init(&p->mutex);xr_cond_init(&p->condition);
    for(unsigned i=0;i<o->workers;++i){s=pool_work(p->resources,sizeof(IndexWorker));if(s!=XR_XIR_OK)break;p->workers[i].pool=p;
        if(!xr_thread_create(&p->workers[i].thread,pool_worker,&p->workers[i])){s=XR_XIR_IO;break;}++p->worker_count;}
    if(s==XR_XIR_OK)*out=p;else xlsp_index_pool_free(p);return s;
}
static XrXirStatus pool_publish(XrLspIndexPool *p,XrLspIndexResult *head,XrLspIndexResult *tail,size_t count) {
    xr_mutex_lock(&p->mutex);XrXirStatus s=XR_XIR_OK;
    if(!p->running)s=XR_XIR_BAD_STAGE;else if(count>UINT64_MAX-p->submitted||count>SIZE_MAX-p->queued)s=XR_XIR_BUDGET;
    else{if(p->tail)p->tail->next=head;else p->head=head;p->tail=tail;p->queued+=count;p->submitted+=count;xr_cond_broadcast(&p->condition);}
    xr_mutex_unlock(&p->mutex);return s;
}
XR_FUNC XrXirStatus xlsp_index_pool_submit(XrLspIndexPool *p,const char *path,const char *uri,uint64_t revision) {
    if(!p||!path)return XR_XIR_BAD_STRUCTURE;XrLspIndexResult *n=NULL;XrXirStatus s=pool_capture(p,path,uri,revision,&n);
    if(s==XR_XIR_OK)s=pool_publish(p,n,n,1);if(s!=XR_XIR_OK)xlsp_index_result_free(n);return s;
}
XR_FUNC XrXirStatus xlsp_index_pool_submit_batch(XrLspIndexPool *p,char *const *paths,size_t count,uint64_t revision) {
    if(!p||(!paths&&count))return XR_XIR_BAD_STRUCTURE;if(!count)return XR_XIR_OK;if(count>SIZE_MAX/sizeof(*paths))return XR_XIR_BUDGET;
    XrLspIndexResult *head=NULL,*tail=NULL;XrXirStatus s=XR_XIR_OK;
    for(size_t i=0;i<count&&s==XR_XIR_OK;++i){s=pool_work(p->resources,sizeof(*paths));if(s!=XR_XIR_OK)break;
        XrLspIndexResult *n=NULL;s=pool_capture(p,paths[i],NULL,revision,&n);if(s!=XR_XIR_OK)break;if(tail)tail->next=n;else head=n;tail=n;}
    if(s==XR_XIR_OK)s=pool_publish(p,head,tail,count);if(s!=XR_XIR_OK)xlsp_index_result_free_list(head);return s;
}
XR_FUNC XrLspIndexResult *xlsp_index_pool_poll(XrLspIndexPool *p) {
    if(!p)return NULL;xr_mutex_lock(&p->mutex);XrLspIndexResult *out=p->results;p->results=p->result_tail=NULL;xr_mutex_unlock(&p->mutex);return out;
}
XR_FUNC bool xlsp_index_pool_is_idle(XrLspIndexPool *p) {
    if(!p)return true;xr_mutex_lock(&p->mutex);bool idle=!p->queued&&!p->active;xr_mutex_unlock(&p->mutex);return idle;
}
XR_FUNC void xlsp_index_pool_get_progress(XrLspIndexPool *p,uint64_t *submitted,uint64_t *completed) {
    if(!p){if(submitted)*submitted=0;if(completed)*completed=0;return;}xr_mutex_lock(&p->mutex);
    if(submitted)*submitted=p->submitted;if(completed)*completed=p->completed;xr_mutex_unlock(&p->mutex);
}
