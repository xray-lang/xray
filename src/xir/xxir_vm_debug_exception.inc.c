/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_debug_exception.inc.c - Own one actual throw before selecting a stop
 */
static XrXirValueStatus debug_exception_prepare(XrXirVmDebug *debug,const XirVmDebugPoint *point,
    const DebugFrame *frame,uint32_t operand,DebugExceptionPlan *output) {
    if (!xir_call_vm_view_current(point->view) || point->view->state!=point->state ||
        operand>=point->layout->slot_count || !frame->known[operand]) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirModule *module=xr_xir_compile_artifact_module(debug->artifact);
    const XrXirFunction *body=&module->functions[point->binding->function];uint32_t offset=point->layout->offsets[operand];
    XrXirType type=xr_xir_operand_type(body,operand);
    if (offset==UINT32_MAX || (uint64_t)offset+sizeof(uint64_t)>point->layout->frame_bytes ||
        (type!=XR_XIR_ERROR && !xr_xir_type_is_enum(module->types,type))) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_work(debug->domain,sizeof(*output)+1)) return XR_XIR_VALUE_LIMIT;
    XrXirValue value={(uint32_t)type,0,xr_xir_scalar_load(point->frame,offset)};
    XrXirValueStatus status=debug_route_error(debug,point->view,&value,&output->error);
    if (status!=XR_XIR_VALUE_OK) return status;
    output->operand=operand;
    status=debug_exception_function(debug,point->view,(XrXirVmDebugLocation){point->binding->function,point->instruction},
        &output->error,&output->local);
    const XirDebugCallFrame *call=xir_debug_calls_current(&debug->calls);
    XrXirVmDebugFrameInfo info={.id=frame->id,.function=point->binding->function,.instruction=point->instruction};
    if (!call || call->id!=frame->id) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (status==XR_XIR_VALUE_OK) status=debug_exception_route(debug,point->view,&info,&output->error,&output->local,&output->route);
    if (status!=XR_XIR_VALUE_OK) return status;
    if (!(debug->exception_filters&XR_XIR_VM_DEBUG_EXCEPTION_ALL) && output->route.kind==XIR_VM_DEBUG_EXCEPTION_ROUTE_UNKNOWN)
        return XR_XIR_VALUE_UNSUPPORTED;
    output->filter=debug_exception_filter(debug->exception_filters,output->route.kind);return XR_XIR_VALUE_OK;
}
