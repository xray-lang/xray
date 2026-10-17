/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_debug_native_source.inc.c - Native source origins without state escape
 */
static XrXirValueStatus debug_native_source_capture(XrXirVmDebug *debug,
    const XirCallNativeSourcePoint *point,bool *stopped) {
    if (!debug_valid(debug) || point->view->instance!=debug->target || !debug->artifact || !debug->mixed || !debug->native_code)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (debug->disabled || debug->faulted || !debug->exception_filters ||
        !debug->source_memory) return XR_XIR_VALUE_OK;
    if (!xr_xir_domain_work(debug->domain,sizeof(*point)+33)) return XR_XIR_VALUE_LIMIT;
    const XrXirModule *module=xr_xir_compile_artifact_module(debug->artifact);
    if (point->entry>=module->function_count || point->instruction>=module->functions[point->entry].instruction_count)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    uint32_t operand=UINT32_MAX;debug_source_throw(debug,point->entry,point->instruction,&operand);
    if (operand==UINT32_MAX) return XR_XIR_VALUE_OK;
    const XrXirFunction *body=&module->functions[point->entry];
    const XrXirFunctionLayout *layout=xr_xir_compile_artifact_layout(debug->artifact,point->entry);
    if (!layout || layout->frame_bytes!=point->bytes || operand>=layout->slot_count ||
        point->values!=(const unsigned char *)point->view->state+debug->native_code->offsets[point->entry] ||
        layout->offsets[operand]==UINT32_MAX || (uint64_t)layout->offsets[operand]+sizeof(uint64_t)>point->bytes)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status=xir_debug_calls_bind(&debug->calls,point->view);
    if (status!=XR_XIR_VALUE_OK) return status;
    const XirDebugCallFrame *call=xir_debug_calls_current(&debug->calls);
    if (!call || call->event.frame!=point->frame || call->event.entry!=point->entry ||
        call->event.code!=XR_XIR_CALL_CODE_NATIVE || call->event.phase!=XR_XIR_CALL_NORMAL)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (debug->skip_once && debug->resume_id==call->id && debug->resume_instruction==point->instruction) {
        debug->skip_once=false;return XR_XIR_VALUE_OK;
    }
    if (xr_xir_vm_debug_paused(debug)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_work(debug->domain,sizeof(DebugExceptionPlan))) return XR_XIR_VALUE_LIMIT;
    XrXirValue value={(uint32_t)xr_xir_operand_type(body,operand),0,xr_xir_scalar_load(point->values,layout->offsets[operand])};
    DebugExceptionPlan exception={.operand=operand,.filter=XR_XIR_VM_DEBUG_EXCEPTION_ALL,
        .native_instruction=point->instruction,.native=true,
        .local={XIR_VM_DEBUG_EXCEPTION_LOCAL_UNKNOWN,point->entry,0,UINT32_MAX},
        .route={.kind=XIR_VM_DEBUG_EXCEPTION_ROUTE_UNKNOWN,.function=UINT32_MAX,.accepted=UINT32_MAX}};
    status=debug_route_error(debug,point->view,&value,&exception.error);
    if (status==XR_XIR_VALUE_OK) status=debug_exception_function(debug,point->view,
        (XrXirVmDebugLocation){point->entry,point->instruction},&exception.error,&exception.local);
    XrXirVmDebugFrameInfo info={.id=call->id,.function=point->entry,.instruction=point->instruction};
    if (status==XR_XIR_VALUE_OK) status=debug_exception_route(debug,point->view,&info,&exception.error,&exception.local,&exception.route);
    if (status==XR_XIR_VALUE_OK && exception.route.kind==XIR_VM_DEBUG_EXCEPTION_ROUTE_UNKNOWN &&
        !(debug->exception_filters&XR_XIR_VM_DEBUG_EXCEPTION_ALL)) status=XR_XIR_VALUE_UNSUPPORTED;
    exception.filter=debug_exception_filter(debug->exception_filters,exception.route.kind);
    if (status==XR_XIR_VALUE_OK && exception.filter) {
        status=capture(debug,point->view,&exception,(XirVmDebugStopCause){.unfiltered=true});
        if (status==XR_XIR_VALUE_OK) *stopped=true;
    }
    xr_xir_value_drop(&exception.error);
    return status;
}
static XrXirCallStatus debug_native_source_before(void *owner,const XirCallNativeSourcePoint *point,bool *stopped) {
    XrXirVmDebug *debug=owner;
    XrXirValueStatus status=debug_native_source_capture(debug,point,stopped);
    if (status==XR_XIR_VALUE_OK) return XR_XIR_CALL_READY;
    xir_vm_debug_faulted(debug,point->view);
    return status==XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM :
        status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_BAD_STATE;
}
static XrXirCallStatus debug_native_source_call(void *owner,const XirCallNativeSourcePending *point) {
    XrXirVmDebug *debug=owner;
    if (!debug_valid(debug) || !point || point->view->instance!=debug->target || !debug->artifact ||
        !debug->mixed || !debug->native_code) return XR_XIR_CALL_BAD_ARGUMENT;
    if (debug->disabled || debug->faulted) return XR_XIR_CALL_READY;
    XrXirValueStatus status=xir_debug_calls_bind(&debug->calls,point->view);
    const XrXirModule *module=xr_xir_compile_artifact_module(debug->artifact);
    const XirDebugCallFrame *call=xir_debug_calls_current(&debug->calls);
    if (status==XR_XIR_VALUE_OK && (!call || call->event.frame!=point->frame || call->event.entry!=point->entry ||
        call->event.code!=XR_XIR_CALL_CODE_NATIVE || call->event.phase!=XR_XIR_CALL_NORMAL ||
        point->entry>=module->function_count || point->callee>=module->function_count ||
        debug->native_code->offsets[point->entry]==UINT32_MAX ||
        point->instruction>=module->functions[point->entry].instruction_count)) status=XR_XIR_VALUE_BAD_ARGUMENT;
    if (status==XR_XIR_VALUE_OK) {
        const XrXirInstruction *op=&module->functions[point->entry].instructions[point->instruction];
        bool invoked=op->op==XR_XIR_INVOKE || op->op==XR_XIR_INVOKE_INDIRECT;
        bool indirect=op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT;
        if (point->invoked!=invoked || (!invoked && op->op!=XR_XIR_CALL && op->op!=XR_XIR_CALL_INDIRECT) ||
            (!indirect && (op->immediate<0 || (uint64_t)op->immediate!=point->callee))) status=XR_XIR_VALUE_BAD_ARGUMENT;
        else status=xir_debug_calls_native_pending(&debug->calls,point->view,point->instruction,point->callee,point->invoked);
    }
    if (status==XR_XIR_VALUE_OK) return XR_XIR_CALL_READY;
    xir_vm_debug_faulted(debug,point->view);
    return status==XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM :
        status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_BAD_STATE;
}
