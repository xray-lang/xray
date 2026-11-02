/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_environment_win.inc.c - Owned Windows environment queries
 *
 * KEY CONCEPT:
 *   OS buffers belong to the active call's actual Domain. Only absence creates
 *   None; provider refusal, cancellation and allocation failure publish nothing.
 */
#include "../os/os_proc.h"

typedef struct XirEnvironmentQuery {
    XrXirCallView *view;
    XrXirDomain *domain;
    XrXirCallStatus status;
} XirEnvironmentQuery;
typedef struct XirEnvironmentBlock { size_t bytes; } XirEnvironmentBlock;

static XrOsIoStatus instance_environment_allocate(void *context,size_t bytes,void **output) {
    XirEnvironmentQuery *query=context;
    if(!query || !output || *output)return XR_OS_IO_BAD_ARGUMENT;
    if(query->status==XR_XIR_CALL_READY)query->status=xr_xir_call_execution_status(query->view);
    if(query->status!=XR_XIR_CALL_READY)return XR_OS_IO_CANCELLED;
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
