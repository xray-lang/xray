/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_view_operation.inc.c - Checked ranges over actual frame view scopes
 *
 * KEY CONCEPT:
 *   VM and generated C11 share root authentication, bounds and fault behavior.
 */
static XrXirValueStatus call_view_root_parameter(XrXirCallView *view,
    const XrXirValue *root,uint32_t *parameter) {
    *parameter=UINT32_MAX;CallFrame *frame=view->activation->top;
    for (uint32_t i=0;i<frame->loan_count;++i) {
        if (!call_cell_work(view->activation,1)) return XR_XIR_VALUE_LIMIT;
        uint32_t p=frame->loans[i].parameter;
        if (frame->arguments[p].type==root->type && frame->arguments[p].payload==root->payload) {
            *parameter=p;break;
        }
    }
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus call_view_root_length(XrXirCallView *view,
    const XrXirValue *root,int64_t *length) {
    XrXirValue value={0};
    XrXirValueStatus status=xr_xir_call_cell_read(view,root,&value);
    if (status!=XR_XIR_VALUE_OK) return status;
    if (value.type==XR_XIR_STRING) {
        const char *bytes=NULL;size_t count=0;
        if (!xr_xir_string_view(&value,&bytes,&count)) status=XR_XIR_VALUE_BAD_ARGUMENT;
        else if (count>(size_t)INT64_MAX) status=XR_XIR_VALUE_LIMIT;
        else *length=(int64_t)count;
    } else status=xr_xir_array_len(&value,xr_xir_call_admission(view),length);
    xr_xir_value_drop(&value);return status;
}
static XrXirValueStatus call_view_range_operation(XrXirCallView *view,
    const XrXirCallViewOperation *request,XrXirValue *output,XrXirFaultDetail *fault) {
    XrXirViewAccess access={0};int64_t length=0;
    bool sub=request->operation==XR_XIR_VIEW_REBORROW;
    CallViewCreation creation={0};
    XrXirValueStatus status=call_view_prepare(view,request->type,output,&creation);
    if (status!=XR_XIR_VALUE_OK || (!sub && !call_view_slot(creation.frame,request->root)))
        return status==XR_XIR_VALUE_OK ? XR_XIR_VALUE_BAD_ARGUMENT : status;
    status=sub ? xr_xir_call_view_access(view,&access) :
        call_view_root_length(view,request->root,&length);
    if (sub && status==XR_XIR_VALUE_OK) status=xr_xir_view_len(request->root,&access,&length);
    if (status!=XR_XIR_VALUE_OK) return status;
    int64_t start=request->start,end=request->to_end ? length : request->end;
    if (start<0 || start>length || end<start || end>length) {
        *fault=(XrXirFaultDetail){XR_XIR_PANIC_BOUNDS,0,start<0 || start>length ? start : end,length};
        return XR_XIR_VALUE_BOUNDS;
    }
    if (sub) return xr_xir_call_view_reborrow(view,request->type,request->root,start,end-start,output);
    uint32_t parameter=UINT32_MAX;
    status=call_view_root_parameter(view,request->root,&parameter);
    if (status!=XR_XIR_VALUE_OK) return status;
    return parameter==UINT32_MAX ? xr_xir_call_view_local(view,request->type,request->root,start,end-start,output) :
        xr_xir_call_view_new(view,request->type,parameter,start,end-start,output);
}
XR_FUNC XrXirValueStatus xr_xir_call_view_operation(XrXirCallView *view,
    const XrXirCallViewOperation *request,XrXirValue *output,XrXirFaultDetail *fault) {
    if (!request || !output || output->type || output->reserved || output->payload || !fault)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    *fault=(XrXirFaultDetail){0};
    if (!xr_xir_call_admission(view) || view->activation->admitting || view->phase!=XR_XIR_CALL_NORMAL ||
        xr_xir_call_execution_status(view)!=XR_XIR_CALL_READY) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (request->operation==XR_XIR_VIEW_END) return xr_xir_call_view_drop(view,request->owned);
    if (!request->root || !xr_xir_value_valid(request->root)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (request->operation==XR_XIR_VIEW_NEW || request->operation==XR_XIR_VIEW_REBORROW)
        return call_view_range_operation(view,request,output,fault);
    XrXirViewAccess access={0};XrXirValueStatus status=xr_xir_call_view_access(view,&access);
    if (status!=XR_XIR_VALUE_OK) return status;
    if (request->operation==XR_XIR_VIEW_LEN) {
        int64_t length=0;status=xr_xir_view_len(request->root,&access,&length);
        if (status==XR_XIR_VALUE_OK) *output=(XrXirValue){XR_XIR_I64,0,length};
        return status;
    }
    if (request->operation==XR_XIR_VIEW_GET)
        return xr_xir_view_get(request->root,request->start,&access,output,fault);
    if (request->operation==XR_XIR_VIEW_SET)
        return xr_xir_view_set(request->root,request->start,request->element,&access,fault);
    if (request->operation==XR_XIR_VIEW_COPY)
        return xr_xir_view_copy(request->type,request->root,&access,output);
    return XR_XIR_VALUE_BAD_ARGUMENT;
}
XR_FUNC XrXirAction xr_xir_call_view_operation_fault(XrXirValueStatus status,XrXirFaultDetail fault) {
    if (status==XR_XIR_VALUE_BOUNDS && fault.code==XR_XIR_PANIC_BOUNDS)
        return xr_xir_call_bounds(fault.index,fault.length);
    XrXirCallStatus code=status==XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM :
        status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_BAD_ARGUMENT;
    return (XrXirAction){XR_XIR_ACTION_FAULT,0,NULL,0,{XR_XIR_I64,0,code},{0},0};
}
