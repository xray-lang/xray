/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_environment_win.inc.c - Owned OS queries and Windows environment snapshots
 *
 * KEY CONCEPT:
 *   OS buffers belong to the active call's actual Domain. Only absence creates
 *   None; provider refusal, cancellation and allocation failure publish nothing.
 */
#include "../os/os_proc.h"
#include "xxir_array.h"
#include "../os/xcwd_owned.h"

typedef struct XirEnvironmentQuery {
    XrXirCallView *view;
    XrXirDomain *domain;
    XrXirCallStatus status;
} XirEnvironmentQuery;
typedef struct XirEnvironmentBlock { size_t bytes; } XirEnvironmentBlock;

static bool instance_environment_current(void *context) {
    XirEnvironmentQuery *query=context;
    if(query->status==XR_XIR_CALL_READY)query->status=xr_xir_call_execution_status(query->view);
    return query->status==XR_XIR_CALL_READY;
}
static XrOsIoStatus instance_environment_allocate(void *context,size_t bytes,void **output) {
    XirEnvironmentQuery *query=context;
    if(!query || !output || *output)return XR_OS_IO_BAD_ARGUMENT;
    if(!instance_environment_current(query))return XR_OS_IO_CANCELLED;
    if(bytes>SIZE_MAX-sizeof(XirEnvironmentBlock))return XR_OS_IO_OVERFLOW;
    size_t total=sizeof(XirEnvironmentBlock)+(bytes?bytes:1);
    if(!xr_xir_domain_work(query->domain,total)) {
        query->status=XR_XIR_CALL_LIMIT;return XR_OS_IO_MEMORY_LIMIT;
    }
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    XirEnvironmentBlock *block=xr_xir_domain_call_allocate(query->domain,total,&status);
    if(!block) {
        query->status=value_call_status(status);
        return status==XR_XIR_VALUE_OOM?XR_OS_IO_OUT_OF_MEMORY:XR_OS_IO_MEMORY_LIMIT;
    }
    block->bytes=total;*output=block+1;return XR_OS_IO_OK;
}
static void instance_environment_free(void *context,void *memory) {
    if(!memory)return;
    XirEnvironmentQuery *query=context;XirEnvironmentBlock *block=(XirEnvironmentBlock *)memory-1;
    xr_xir_domain_call_deallocate(query->domain,block,block->bytes);
}
static XrXirCallStatus instance_environment_status(XrOsIoStatus status) {
    switch(status) {
    case XR_OS_IO_OK:return XR_XIR_CALL_READY;
    case XR_OS_IO_OUT_OF_MEMORY:return XR_XIR_CALL_OOM;
    case XR_OS_IO_OVERFLOW:case XR_OS_IO_MEMORY_LIMIT:return XR_XIR_CALL_LIMIT;
    case XR_OS_IO_UNSUPPORTED:return XR_XIR_CALL_UNSUPPORTED;
    default:return XR_XIR_CALL_HOST_ERROR;
    }
}
#if defined(XR_OS_WINDOWS)
static XrXirCallStatus instance_environment_get(XrXirCallView *view,XrXirInstance *instance,
    const XrXirValue *name,XrXirType type,XrXirValue *output) {
    XrXirCallStatus current=xr_xir_call_execution_status(view);
    if(current!=XR_XIR_CALL_READY)return current;
    const char *bytes=NULL;size_t length=0;
    if(!xr_xir_string_view(name,&bytes,&length))return XR_XIR_CALL_BAD_ARGUMENT;
    if(length==SIZE_MAX || !xr_xir_domain_work(instance->domain,length+1))return XR_XIR_CALL_LIMIT;
    if(!length || memchr(bytes,0,length) || memchr(bytes,'=',length))return XR_XIR_CALL_HOST_ERROR;
    XirEnvironmentQuery query={view,instance->domain,XR_XIR_CALL_READY};
    XrOsIoPolicy policy={&query,instance_environment_allocate,instance_environment_free};
    void *owned_name=NULL;char *raw=NULL;XrXirValue text={0};
    XrOsIoStatus queried=instance_environment_allocate(&query,length+1,&owned_name);
    if(queried==XR_OS_IO_OK) {
        memcpy(owned_name,bytes,length);((char *)owned_name)[length]=0;
        queried=xr_os_io_environment_get(&policy,owned_name,&raw);
    }
    instance_environment_free(&query,owned_name);
    XrXirCallStatus status=query.status;
    if(status==XR_XIR_CALL_READY)status=xr_xir_call_execution_status(view);
    if(status==XR_XIR_CALL_READY && queried==XR_OS_IO_NOT_FOUND && !raw) {
        status=value_call_status(xr_xir_nullable_new(type,NULL,xr_xir_call_admission(view),output));
    } else if(status==XR_XIR_CALL_READY) {
        status=instance_environment_status(queried);
        if(status==XR_XIR_CALL_READY && !raw)status=XR_XIR_CALL_HOST_ERROR;
        if(status==XR_XIR_CALL_READY) {
            XrXirValueStatus copied=xr_xir_string_new(instance->domain,raw,strlen(raw),&text);
            status=copied==XR_XIR_VALUE_BAD_UTF8?XR_XIR_CALL_HOST_ERROR:value_call_status(copied);
        }
        if(status==XR_XIR_CALL_READY)
            status=value_call_status(xr_xir_nullable_new(type,&text,xr_xir_call_admission(view),output));
    }
    instance_environment_free(&query,raw);xr_xir_value_drop(&text);return status;
}

static XrXirCallStatus instance_environment_set(XrXirCallView *view,XrXirInstance *instance,
    const XrXirValue *arguments,bool remove,XrXirValue *output) {
    XirEnvironmentQuery query={view,instance->domain,XR_XIR_CALL_READY};
    if(!instance_environment_current(&query))return query.status;
    const char *bytes[2]={NULL,NULL};size_t lengths[2]={0,0};
    for(uint32_t p=0;p<(remove?1u:2u);++p) {
        if(!xr_xir_string_view(arguments+p,&bytes[p],&lengths[p]))return XR_XIR_CALL_BAD_ARGUMENT;
        if(lengths[p]==SIZE_MAX || !xr_xir_domain_work(instance->domain,lengths[p]+1))return XR_XIR_CALL_LIMIT;
    }
    if(!lengths[0] || memchr(bytes[0],'=',lengths[0]) || memchr(bytes[0],0,lengths[0]) || (!remove && memchr(bytes[1],0,lengths[1]))) {
        *output=(XrXirValue){XR_XIR_BOOL,0,false};return XR_XIR_CALL_READY;
    }
    XrOsIoPolicy policy={&query,instance_environment_allocate,instance_environment_free};
    void *owned[2]={NULL,NULL};XrOsIoStatus called=XR_OS_IO_OK;bool changed=false;
    for(uint32_t p=0;p<(remove?1u:2u) && called==XR_OS_IO_OK;++p) {
        called=instance_environment_allocate(&query,lengths[p]+1,&owned[p]);
        if(called==XR_OS_IO_OK) {memcpy(owned[p],bytes[p],lengths[p]);((char *)owned[p])[lengths[p]]=0;}
    }
    if(called==XR_OS_IO_OK)called=xr_os_io_environment_set(&policy,owned[0],owned[1],instance_environment_current,&changed);
    instance_environment_free(&query,owned[1]);instance_environment_free(&query,owned[0]);
    if(!instance_environment_current(&query))return query.status;
    XrXirCallStatus status=instance_environment_status(called);
    if(status==XR_XIR_CALL_READY)*output=(XrXirValue){XR_XIR_BOOL,0,changed};
    return status;
}

/* Failed OS closes belong to the root, which outlives every child generation.
 * The node never retains a callback view or its synchronous policy context. */
struct XirEnvironmentSnapshotOwner {
    XrOsEnvironmentSnapshot snapshot;
    XirEnvironmentSnapshotOwner *next;
};
typedef struct XirEnvironmentSnapshotQuery {
    XirEnvironmentQuery query;
    XrXirValue *values;
    size_t count,capacity;
} XirEnvironmentSnapshotQuery;

static bool instance_environment_snapshot_close(XrXirInstance *root,XirEnvironmentSnapshotOwner *owner) {
    if(xr_os_io_environment_snapshot_close(&owner->snapshot)!=XR_OS_IO_OK)return false;
    XirEnvironmentSnapshotOwner **link=&root->environment_snapshots;
    while(*link && *link!=owner)link=&(*link)->next;
    XR_CHECK(*link==owner,"snapshot close requires its root-owned node");
    *link=owner->next;xr_xir_domain_metadata_deallocate(root->domain,owner,sizeof(*owner));return true;
}
static bool instance_environment_snapshots_close(XrXirInstance *instance) {
    XR_CHECK(instance==instance_root(instance),"only the root discharges retained environment snapshots");
    bool complete=true;
    for(XirEnvironmentSnapshotOwner *owner=instance->environment_snapshots;owner;) {
        XirEnvironmentSnapshotOwner *next=owner->next;
        if(!instance_environment_snapshot_close(instance,owner))complete=false;
        owner=next;
    }
    return complete;
}
static bool instance_environment_snapshot_current(void *context,size_t work) {
    XirEnvironmentSnapshotQuery *query=context;
    if(!instance_environment_current(&query->query))return false;
    if(!xr_xir_domain_work(query->query.domain,work))query->query.status=XR_XIR_CALL_LIMIT;
    return query->query.status==XR_XIR_CALL_READY;
}
static XrOsIoStatus instance_environment_snapshot_entry(void *context,const char *text,size_t length) {
    XirEnvironmentSnapshotQuery *query=context;XirEnvironmentQuery *work=&query->query;
    if(!instance_environment_current(work))return XR_OS_IO_CANCELLED;
    if(query->count==query->capacity) {
        if(query->capacity>SIZE_MAX/2 || (query->capacity?query->capacity*2:8)>SIZE_MAX/sizeof(*query->values)) {
            work->status=XR_XIR_CALL_LIMIT;return XR_OS_IO_OVERFLOW;
        }
        size_t capacity=query->capacity?query->capacity*2:8;void *memory=NULL;
        XrOsIoStatus status=instance_environment_allocate(work,capacity*sizeof(*query->values),&memory);
        if(status!=XR_OS_IO_OK)return status;
        if(query->count)memcpy(memory,query->values,query->count*sizeof(*query->values));
        instance_environment_free(work,query->values);query->values=memory;query->capacity=capacity;
    }
    XrXirValueStatus status=xr_xir_string_new(work->domain,text,length,query->values+query->count);
    if(status!=XR_XIR_VALUE_OK) {
        work->status=status==XR_XIR_VALUE_BAD_UTF8?XR_XIR_CALL_HOST_ERROR:value_call_status(status);
        return status==XR_XIR_VALUE_OOM?XR_OS_IO_OUT_OF_MEMORY:XR_OS_IO_IO;
    }
    ++query->count;return XR_OS_IO_OK;
}
static XrXirCallStatus instance_environment_snapshot(XrXirCallView *view,XrXirInstance *instance,
    XrXirType type,XrXirValue *output) {
    XirEnvironmentSnapshotQuery query={.query={view,instance->domain,XR_XIR_CALL_READY}};
    if(!instance_environment_current(&query.query))return query.query.status;
    XrXirInstance *root=instance_root(instance);XrXirCallStatus status=XR_XIR_CALL_READY;
    if(!xr_xir_domain_work(root->domain,sizeof(XirEnvironmentSnapshotOwner)))return XR_XIR_CALL_LIMIT;
    XirEnvironmentSnapshotOwner *owner=instance_metadata_allocate(root->domain,sizeof(*owner),&status);
    if(!owner)return status;
    owner->next=root->environment_snapshots;root->environment_snapshots=owner;
    XrOsIoPolicy policy={&query,instance_environment_allocate,instance_environment_free};
    XrOsIoStatus observed=xr_os_io_environment_snapshot_open(&owner->snapshot);
    if(observed==XR_OS_IO_OK)observed=xr_os_io_environment_snapshot_each(&policy,&owner->snapshot,
        instance_environment_snapshot_current,instance_environment_snapshot_entry);
    status=query.query.status;XrXirValue result={0};
    if(status==XR_XIR_CALL_READY)status=xr_xir_call_execution_status(view);
    if(status==XR_XIR_CALL_READY)status=instance_environment_status(observed);
    if(status==XR_XIR_CALL_READY)
        status=value_call_status(xr_xir_array_new(type,query.values,query.count,xr_xir_call_admission(view),&result));
    for(size_t i=0;i<query.count;++i)xr_xir_value_drop(query.values+i);
    instance_environment_free(&query.query,query.values);
    if(!instance_environment_snapshot_close(root,owner) && status==XR_XIR_CALL_READY)status=XR_XIR_CALL_HOST_ERROR;
    if(status==XR_XIR_CALL_READY)status=xr_xir_call_execution_status(view);
    if(status==XR_XIR_CALL_READY) { *output=result;result=(XrXirValue){0}; }
    xr_xir_value_drop(&result);return status;
}

#endif // XR_OS_WINDOWS

static XrXirCallStatus instance_current_directory(XrXirCallView *view,XrXirInstance *instance,
    XrXirValue *output) {
    XirEnvironmentQuery query={view,instance->domain,XR_XIR_CALL_READY};
    if(!instance_environment_current(&query))return query.status;
    XrOsIoPolicy policy={&query,instance_environment_allocate,instance_environment_free};char *raw=NULL;
    XrOsIoStatus queried=xr_os_io_getcwd_owned(&policy,&raw);XrXirValue result={0};
    XrXirCallStatus status=query.status;
    if(status==XR_XIR_CALL_READY)status=instance_environment_status(queried);
    if(status==XR_XIR_CALL_READY)status=xr_xir_call_execution_status(view);
    if(status==XR_XIR_CALL_READY && !raw)status=XR_XIR_CALL_HOST_ERROR;
    if(status==XR_XIR_CALL_READY) {
        XrXirValueStatus copied=xr_xir_string_new(instance->domain,raw,strlen(raw),&result);
        status=copied==XR_XIR_VALUE_BAD_UTF8?XR_XIR_CALL_HOST_ERROR:value_call_status(copied);
    }
    instance_environment_free(&query,raw);
    if(status==XR_XIR_CALL_READY)status=xr_xir_call_execution_status(view);
    if(status==XR_XIR_CALL_READY){*output=result;result=(XrXirValue){0};}
    xr_xir_value_drop(&result);return status;
}

#if defined(XR_OS_WINDOWS)
static XrXirCallStatus instance_hostname(XrXirCallView *view,XrXirInstance *instance,
    XrXirValue *output) {
    XirEnvironmentQuery query={view,instance->domain,XR_XIR_CALL_READY};
    if(!instance_environment_current(&query))return query.status;
    XrOsIoPolicy policy={&query,instance_environment_allocate,instance_environment_free};char *raw=NULL;
    XrOsIoStatus queried=xr_os_io_hostname(&policy,&raw);XrXirValue result={0};
    XrXirCallStatus status=query.status;
    if(status==XR_XIR_CALL_READY)status=instance_environment_status(queried);
    if(status==XR_XIR_CALL_READY)status=xr_xir_call_execution_status(view);
    if(status==XR_XIR_CALL_READY && !raw)status=XR_XIR_CALL_HOST_ERROR;
    if(status==XR_XIR_CALL_READY) {
        XrXirValueStatus copied=xr_xir_string_new(instance->domain,raw,strlen(raw),&result);
        status=copied==XR_XIR_VALUE_BAD_UTF8?XR_XIR_CALL_HOST_ERROR:value_call_status(copied);
    }
    instance_environment_free(&query,raw);
    if(status==XR_XIR_CALL_READY)status=xr_xir_call_execution_status(view);
    if(status==XR_XIR_CALL_READY){*output=result;result=(XrXirValue){0};}
    xr_xir_value_drop(&result);return status;
}
static XrXirCallStatus instance_system_username(XrXirCallView *view,XrXirInstance *instance,
    XrXirType type,XrXirValue *output) {
    XirEnvironmentQuery query={view,instance->domain,XR_XIR_CALL_READY};
    if(!instance_environment_current(&query))return query.status;
    XrOsIoPolicy policy={&query,instance_environment_allocate,instance_environment_free};char *raw=NULL;
    XrOsIoStatus queried=xr_os_io_system_username(&policy,&raw);XrXirValue text={0},result={0};
    XrXirCallStatus status=query.status;
    if(status==XR_XIR_CALL_READY)status=instance_environment_status(queried);
    if(status==XR_XIR_CALL_READY)status=xr_xir_call_execution_status(view);
    if(status==XR_XIR_CALL_READY && !raw)status=XR_XIR_CALL_HOST_ERROR;
    if(status==XR_XIR_CALL_READY) {
        XrXirValueStatus copied=xr_xir_string_new(instance->domain,raw,strlen(raw),&text);
        status=copied==XR_XIR_VALUE_BAD_UTF8?XR_XIR_CALL_HOST_ERROR:value_call_status(copied);
    }
    instance_environment_free(&query,raw);
    if(status==XR_XIR_CALL_READY)
        status=value_call_status(xr_xir_nullable_new(type,&text,xr_xir_call_admission(view),&result));
    xr_xir_value_drop(&text);
    if(status==XR_XIR_CALL_READY)status=xr_xir_call_execution_status(view);
    if(status==XR_XIR_CALL_READY){*output=result;result=(XrXirValue){0};}
    xr_xir_value_drop(&result);return status;
}
static XrXirCallStatus instance_system_homedir(XrXirCallView *view,XrXirInstance *instance,
    XrXirType type,XrXirValue *output) {
    XirEnvironmentQuery query={view,instance->domain,XR_XIR_CALL_READY};
    if(!instance_environment_current(&query))return query.status;
    XrOsIoPolicy policy={&query,instance_environment_allocate,instance_environment_free};char *raw=NULL;
    XrOsIoStatus queried=xr_os_io_system_homedir(&policy,&raw);XrXirValue text={0},result={0};
    XrXirCallStatus status=query.status;
    if(status==XR_XIR_CALL_READY)status=instance_environment_status(queried);
    if(status==XR_XIR_CALL_READY)status=xr_xir_call_execution_status(view);
    if(status==XR_XIR_CALL_READY && (!raw || !*raw))status=XR_XIR_CALL_HOST_ERROR;
    if(status==XR_XIR_CALL_READY) {
        XrXirValueStatus copied=xr_xir_string_new(instance->domain,raw,strlen(raw),&text);
        status=copied==XR_XIR_VALUE_BAD_UTF8?XR_XIR_CALL_HOST_ERROR:value_call_status(copied);
    }
    instance_environment_free(&query,raw);
    if(status==XR_XIR_CALL_READY)
        status=value_call_status(xr_xir_nullable_new(type,&text,xr_xir_call_admission(view),&result));
    xr_xir_value_drop(&text);
    if(status==XR_XIR_CALL_READY)status=xr_xir_call_execution_status(view);
    if(status==XR_XIR_CALL_READY){*output=result;result=(XrXirValue){0};}
    xr_xir_value_drop(&result);return status;
}
#endif // XR_OS_WINDOWS
