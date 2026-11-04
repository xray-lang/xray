/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_pipe_owner.inc.c - Root endpoint and per-frame operation owners
 */
#include "../os/os_pipe_owned.h"
#include "xxir_array.h"
#include "xxir_tuple.h"

struct XirPipeOwner {
    XirPipeOwner *next;
    XrOsPipeOwner owner;
    int64_t tokens[2];
    uint64_t references;
    bool published;
};
struct XirPipeFrame {
    XirPipeFrame *next;
    XirPipeOwner *pipe;
    XrOsPipeOperation operation;
    XrXirCall *call;
    XrXirValue *frame;
    XrXirInstance *generation;
    uint64_t generation_id;
    uint32_t entry,id;
    XirCallSignalOrigin origin;
    XrOsPipeProgress observed;
    XrXirCallStatus retirement_failure;
    bool begun,terminal,opened,retiring;
};
typedef struct XirPipeBlock {size_t bytes,padding;} XirPipeBlock;
typedef struct XirPipeQuery {
    XrXirCallView *view;
    XrXirDomain *domain;
    XrXirCallStatus status;
} XirPipeQuery;

static XrXirCallStatus instance_pipe_status(XrOsIoStatus status) {
    switch(status) {
    case XR_OS_IO_OK:return XR_XIR_CALL_READY;
    case XR_OS_IO_OUT_OF_MEMORY:return XR_XIR_CALL_OOM;
    case XR_OS_IO_MEMORY_LIMIT:case XR_OS_IO_OVERFLOW:return XR_XIR_CALL_LIMIT;
    case XR_OS_IO_CANCELLED:return XR_XIR_CALL_CANCELLED;
    case XR_OS_IO_UNSUPPORTED:return XR_XIR_CALL_UNSUPPORTED;
    default:return XR_XIR_CALL_HOST_ERROR;
    }
}
static bool instance_pipe_current(void *context) {
    XirPipeQuery *query=context;
    if(query->status==XR_XIR_CALL_READY)query->status=xr_xir_call_execution_status(query->view);
    if(query->status==XR_XIR_CALL_READY && !xr_xir_domain_work(query->domain,1))query->status=XR_XIR_CALL_LIMIT;
    return query->status==XR_XIR_CALL_READY;
}
/* The policy retains only the stable Domain. Request queries are never stored
 * in an endpoint, a worker, or a close-retry owner. */
static XrOsIoStatus instance_pipe_allocate(void *context,size_t bytes,void **output) {
    XrXirDomain *domain=context;
    if(!domain || !output || *output)return XR_OS_IO_BAD_ARGUMENT;
    if(bytes>SIZE_MAX-sizeof(XirPipeBlock))return XR_OS_IO_OVERFLOW;
    size_t total=sizeof(XirPipeBlock)+(bytes?bytes:1);
    if(!xr_xir_domain_work(domain,total))return XR_OS_IO_MEMORY_LIMIT;
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    XirPipeBlock *block=xr_xir_domain_call_allocate(domain,total,&status);
    if(!block)return status==XR_XIR_VALUE_OOM?XR_OS_IO_OUT_OF_MEMORY:
        status==XR_XIR_VALUE_LIMIT?XR_OS_IO_MEMORY_LIMIT:XR_OS_IO_BAD_ARGUMENT;
    block->bytes=total;*output=block+1;return XR_OS_IO_OK;
}
static void instance_pipe_free(void *context,void *memory) {
    if(!memory)return;
    XirPipeBlock *block=(XirPipeBlock *)memory-1;
    xr_xir_domain_call_deallocate(context,block,block->bytes);
}
static void instance_pipe_frame_unlink(XrXirInstance *root,XirPipeFrame *frame) {
    XR_CHECK(!frame->frame && !frame->call && !frame->generation && !frame->operation.state,
        "only a detached and discharged pipe operation can retire");
    XirPipeFrame **link=&root->pipe_frames;
    while(*link && *link!=frame)link=&(*link)->next;
    XR_CHECK(*link==frame,"pipe retirement retains its actual Root node");
    *link=frame->next;
    if(frame->pipe) {
        XR_CHECK(frame->pipe->references,"a pipe frame owns one endpoint-owner reference");
        --frame->pipe->references;
    }
    xr_xir_domain_metadata_deallocate(root->domain,frame,sizeof(*frame));
}
static bool instance_pipe_owners_close(XrXirInstance *root,bool all) {
    XR_CHECK(root==instance_root(root) && root->driving,"pipe drain retains the busy Root");
    bool complete=true;
    for(XirPipeFrame *frame=root->pipe_frames;frame;) {
        XirPipeFrame *next=frame->next;
        if(frame->frame) {if(all)complete=false;frame=next;continue;}
        bool closed=!frame->operation.state;
        XrOsIoStatus status=closed?XR_OS_IO_OK:xr_os_io_pipe_operation_close(&frame->operation,&closed);
        if(status==XR_OS_IO_OK && closed)instance_pipe_frame_unlink(root,frame);
        else complete=false;
        frame=next;
    }
    XirPipeOwner **link=&root->pipe_owners;
    while(*link) {
        XirPipeOwner *pipe=*link;
        if(!all && pipe->published && (pipe->tokens[0] || pipe->tokens[1])) {link=&pipe->next;continue;}
        if(pipe->references) {complete=false;link=&pipe->next;continue;}
        bool closed=!pipe->owner.state;
        XrOsIoStatus status=closed?XR_OS_IO_OK:xr_os_io_pipe_dispose(&pipe->owner,&closed);
        if(status==XR_OS_IO_OK && closed) {
            *link=pipe->next;xr_xir_domain_metadata_deallocate(root->domain,pipe,sizeof(*pipe));
        } else {complete=false;link=&pipe->next;}
    }
    return complete;
}
static XirPipeFrame *instance_pipe_frame_find(XrXirInstance *root,const XrXirCallView *view) {
    for(XirPipeFrame *frame=root->pipe_frames;frame;frame=frame->next)
        if(frame->frame==view->state && frame->call==view->activation)return frame;
    return NULL;
}
XR_FUNC bool xir_instance_pipe_retirement_pending(const void *instance,const XrXirCall *call,
    const XrXirValueAdmission *admission) {
    if(!instance || !admission || admission->function!=admit_function_binding)return false;
    const XrXirInstance *root=instance_root((XrXirInstance *)instance);
    for(const XirPipeFrame *frame=root->pipe_frames;frame;frame=frame->next)
        if(frame->call==call && frame->retiring && (frame->operation.state ||
            (frame->pipe && !frame->pipe->published && frame->pipe->owner.state)))return true;
    return false;
}
XR_FUNC bool xir_instance_pipe_retire(XrXirCallView *view,XrXirCallStatus *failure) {
    uint32_t entry=UINT32_MAX;
    const XrXirValueAdmission *admission=xir_call_retirement_admission(view,&entry);
    if(!admission || admission->function!=admit_function_binding || !view->instance)return true;
    XrXirInstance *root=instance_root(view->instance);
    XirPipeFrame *frame=instance_pipe_frame_find(root,view);
    if(!frame)return true;
    XR_CHECK(entry==frame->entry && frame->generation && admission->context==frame->generation &&
        frame->generation_id==frame->generation->generation_id &&
        frame->generation->domain==root->domain && frame->generation->program->arena==view->arena,
        "pipe retirement borrows its actual owned Call frame and generation");
    frame->retiring=true;
    bool closed=!frame->operation.state;
    XrOsIoStatus status=closed?XR_OS_IO_OK:xr_os_io_pipe_operation_close(&frame->operation,&closed);
    if(status==XR_OS_IO_OK && closed && frame->pipe && !frame->pipe->published) {
        XR_CHECK(frame->pipe->references==1,"an unpublished endpoint pair belongs only to its opening frame");
        closed=!frame->pipe->owner.state;
        if(!closed)status=xr_os_io_pipe_dispose(&frame->pipe->owner,&closed);
    }
    if(status!=XR_OS_IO_OK && frame->retirement_failure==XR_XIR_CALL_READY)
        frame->retirement_failure=instance_pipe_status(status);
    if(status!=XR_OS_IO_OK || !closed)return false;
    *failure=frame->retirement_failure;return true;
}
/* The trusted release view cannot request new admission. It can only detach
 * its exact existing coordinates before the Call frees that physical frame. */
static void instance_pipe_frame_release(XrXirCallView *view) {
    if(!view || !view->instance)return;
    XrXirInstance *root=instance_root(view->instance);
    XirPipeFrame *frame=instance_pipe_frame_find(root,view);
    if(!frame)return;
    XR_CHECK(frame->generation && frame->generation->domain==root->domain &&
        frame->generation_id==frame->generation->generation_id &&
        frame->generation->program->arena==view->arena,"pipe release retains its actual generation metadata");
    frame->call=NULL;frame->frame=NULL;frame->generation=NULL;frame->generation_id=0;
    frame->entry=frame->id=0;frame->origin=(XirCallSignalOrigin){0};
    XR_CHECK(!frame->operation.state,"a Call frame retires only after its worker has joined and closed");
    bool driving=root->driving;root->driving=true;
    (void)instance_pipe_owners_close(root,false);root->driving=driving;
}
static XrXirCallStatus instance_owned_tokens(uint32_t count,int64_t *tokens) {
    static _Atomic(uint64_t) sequence;
    uint64_t old=atomic_load_explicit(&sequence,memory_order_relaxed);
    for(;;) {
        if(!count||old>(uint64_t)INT64_MAX-count)return XR_XIR_CALL_LIMIT;
        if(atomic_compare_exchange_weak_explicit(&sequence,&old,old+count,memory_order_relaxed,memory_order_relaxed)) {
            for(uint32_t i=0;i<count;++i)tokens[i]=(int64_t)(old+i+1);return XR_XIR_CALL_READY;
        }
    }
}
static XrXirCallStatus instance_pipe_token_pair(int64_t tokens[2]) {return instance_owned_tokens(2,tokens);}
static XirPipeOwner *instance_pipe_lookup(XrXirInstance *root,int64_t token,uint32_t *end,XirPipeQuery *query) {
    if(token<=0)return NULL;
    for(XirPipeOwner *pipe=root->pipe_owners;pipe;pipe=pipe->next) {
        if(!instance_pipe_current(query))return NULL;
        for(uint32_t p=0;p<2;++p)if(pipe->published && pipe->tokens[p]==token) {*end=p;return pipe;}
    }
    return NULL;
}
/* A token identifies only a live endpoint in this Root. The optional exact
 * role prevents a cleanup pair from closing a different kind of endpoint. */
static XrXirCallStatus instance_pipe_close_endpoint(XrXirCallView *view,XrXirInstance *instance,
    int64_t token,uint32_t expected,XirPipeOwner **held,bool *closed) {
    if(!held||*held||!closed)return XR_XIR_CALL_BAD_STATE;
    XirPipeQuery current={view,instance->domain,XR_XIR_CALL_READY};uint32_t end=0;
    XirPipeOwner *pipe=instance_pipe_lookup(instance_root(instance),token,&end,&current);
    if(current.status!=XR_XIR_CALL_READY)return current.status;
    *closed=false;
    if(!pipe||(expected!=UINT32_MAX&&end!=expected))return XR_XIR_CALL_READY;
    if(pipe->references==UINT64_MAX)return XR_XIR_CALL_LIMIT;
    *held=pipe;++pipe->references;
    XrXirCallStatus status=instance_pipe_status(xr_os_io_pipe_close_end(&pipe->owner,(XrOsPipeEnd)end,closed));
    if(status==XR_XIR_CALL_READY&&*closed)pipe->tokens[end]=0;
    return status;
}
