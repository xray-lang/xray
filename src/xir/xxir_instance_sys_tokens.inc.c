/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_sys_tokens.inc.c - Governed queries of process-owned thread tokens
 *
 * KEY CONCEPT:
 *   The current Call supplies cancellation and work observation, never the
 *   lifetime or allocator of the process-owned thread identity service.
 */
#include "../os/os_thread_token.h"

typedef struct XirThreadTokenQuery {
    XrXirCallView *view;
    XrXirDomain *domain;
    XrXirCallStatus status;
} XirThreadTokenQuery;

static bool instance_thread_token_current(void *context) {
    XirThreadTokenQuery *query=context;
    if(query->status==XR_XIR_CALL_READY)query->status=xr_xir_call_execution_status(query->view);
    if(query->status==XR_XIR_CALL_READY && !xr_xir_domain_work(query->domain,1))query->status=XR_XIR_CALL_LIMIT;
    return query->status==XR_XIR_CALL_READY;
}
static XrXirCallStatus instance_thread_token_status(XrOsIoStatus status) {
    switch(status) {
    case XR_OS_IO_OK:return XR_XIR_CALL_READY;
    case XR_OS_IO_OUT_OF_MEMORY:return XR_XIR_CALL_OOM;
    case XR_OS_IO_OVERFLOW:case XR_OS_IO_MEMORY_LIMIT:return XR_XIR_CALL_LIMIT;
    case XR_OS_IO_CANCELLED:return XR_XIR_CALL_CANCELLED;
    case XR_OS_IO_UNSUPPORTED:return XR_XIR_CALL_UNSUPPORTED;
    default:return XR_XIR_CALL_HOST_ERROR;
    }
}
static XrXirCallStatus instance_sys_token_call(XrXirCallView *view,XrXirInstance *instance,
    uint32_t id,XrXirValue *output) {
    if(!view || !instance || !output || !value_unit(*output) || !xir_sys_thread_token_id(id) ||
        view->phase!=XR_XIR_CALL_NORMAL || view_instance(view)!=instance)return XR_XIR_CALL_BAD_STATE;
    const XrXirProgram *program=instance->program;
    uint32_t index=xr_xir_call_current_entry(view->activation);
    const XrXirModule *module=instance_method_module(instance);
    if(!module || index>=module->function_count || index>=program->entry_count ||
        !module->declarations || !module->declarations->functions ||
        module->declarations->functions[index].native_callable!=id ||
        !xir_sys_native_scope(module->declarations,index,module->function_count) ||
        xir_native_callable_verify(module,index)!=XR_XIR_OK)return XR_XIR_CALL_BAD_STATE;
    bool alive=id==XR_NATIVE_CALLABLE_SYS_THREAD_LOCAL_ALIVE;
    if(view->argument_count!=(alive?1u:0u) || (alive && (!view->arguments ||
        !xr_xir_value_argument(view->arguments,view->arena,XR_XIR_I64))))return XR_XIR_CALL_BAD_ARGUMENT;
    XirThreadTokenQuery query={view,instance->domain,XR_XIR_CALL_READY};
    if(!instance_thread_token_current(&query))return query.status;
    XrOsThreadTokenQuery request={&query,instance_thread_token_current};
    int64_t token=0;bool present=false;
    XrOsIoStatus queried=alive?xr_os_io_thread_token_alive(&request,view->arguments[0].payload,&present):
        xr_os_io_thread_token_current(&request,&token);
    if(!instance_thread_token_current(&query))return query.status;
    XrXirCallStatus status=instance_thread_token_status(queried);
    if(status==XR_XIR_CALL_READY) {
        if(!alive && token<=0)return XR_XIR_CALL_HOST_ERROR;
        *output=(XrXirValue){alive?XR_XIR_BOOL:XR_XIR_I64,0,alive?(present?1:0):token};
    }
    return status;
}
