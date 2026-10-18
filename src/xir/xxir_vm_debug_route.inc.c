/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_debug_route.inc.c - Match authenticated synchronous VM and native ancestors
 */
static XrXirValueStatus debug_route_error(XrXirVmDebug *debug,XrXirCallView *view,
    const XrXirValue *input,XrXirValue *output) {
    XrXirValueAdmission *actual=xr_xir_call_admission(view);
    if (!actual) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirDomainBudgetStats budget=xr_xir_domain_budget_stats(debug->domain);
    uint64_t available=budget.bound ? budget.work_limit-budget.work : UINT64_MAX;
    XrXirValueAdmission admission=*actual;admission.work=available<actual->work ? available : actual->work;
    uint64_t work=admission.work;XrXirValueStatus status=xr_xir_error_erase(input,&admission,output);
    uint64_t consumed=work-admission.work;
    bool paid=consumed<=actual->work && xr_xir_domain_work(debug->domain,consumed);
    if (consumed<=actual->work) actual->work-=consumed;
    return status==XR_XIR_VALUE_OK && !paid ? XR_XIR_VALUE_LIMIT : status;
}
static XrXirValueStatus debug_route_escape(XrXirVmDebug *debug,XrXirCallView *view,
    const XirDebugCallFrame *call,const XirDebugCallFrame *parent,XirVmDebugExceptionRoute *output,uint32_t ancestors) {
    if (!xr_xir_domain_work(debug->domain,sizeof(XirInstanceDebugActivation)+1)) return XR_XIR_VALUE_LIMIT;
    XirInstanceDebugActivation activation={0};XrXirCallStatus status=xir_instance_debug_activation(view,&activation);
    if (status!=XR_XIR_CALL_READY) return status==XR_XIR_CALL_LIMIT ? XR_XIR_VALUE_LIMIT : XR_XIR_VALUE_BAD_ARGUMENT;
    bool task=activation.child && !parent && !call->event.parent && call->event.depth==1;
    bool root=!activation.child && parent && parent->event.entry==activation.driver &&
        activation.driver==debug->calls.functions && !parent->event.parent && parent->event.depth==1 &&
        parent->event.code==XR_XIR_CALL_CODE_NATIVE && parent->event.phase==XR_XIR_CALL_NORMAL && call->event.depth==2;
    if (task || root) *output=(XirVmDebugExceptionRoute){task ? XIR_VM_DEBUG_EXCEPTION_ROUTE_TASK_ESCAPE :
        XIR_VM_DEBUG_EXCEPTION_ROUTE_ROOT_ESCAPE,call->event.entry,0,UINT32_MAX,call->id,ancestors};
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus debug_exception_route(XrXirVmDebug *debug,XrXirCallView *view,
    const XrXirVmDebugFrameInfo *frame,const XrXirValue *error,const XirVmDebugExceptionLocal *local,XirVmDebugExceptionRoute *output) {
    XirVmDebugExceptionRoute result={.function=UINT32_MAX,.accepted=UINT32_MAX};
    const XirDebugCallFrame *call=xir_debug_calls_current(&debug->calls);
    if (!call || call->event.entry!=frame->function || call->id!=frame->id)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XirVmDebugExceptionLocal selection=*local;XrXirValueStatus status=XR_XIR_VALUE_OK;
    uint32_t ancestors=0;
    while (selection.kind!=XIR_VM_DEBUG_EXCEPTION_LOCAL_UNKNOWN) {
        if (selection.kind==XIR_VM_DEBUG_EXCEPTION_LOCAL_MATCH) {
            result=(XirVmDebugExceptionRoute){XIR_VM_DEBUG_EXCEPTION_ROUTE_SELECTED,
                selection.function,selection.handler,selection.accepted,call->id,ancestors};break;
        }
        if (!xr_xir_domain_work(debug->domain,(uint64_t)debug->calls.capacity+debug->count+call->event.depth+1)) {
            status=XR_XIR_VALUE_LIMIT;break;
        }
        const XirDebugCallFrame *parent=xir_debug_calls_parent(&debug->calls,call);
        if (!parent || parent->event.entry==debug->calls.functions) {
            status=debug_route_escape(debug,view,call,parent,&result,ancestors);break;
        }
        if (parent->thread!=call->thread || parent->event.depth+1!=call->event.depth || ancestors>=debug->capacity) {
            status=XR_XIR_VALUE_BAD_ARGUMENT;break;
        }
        uint32_t function=UINT32_MAX,instruction=UINT32_MAX,callee=UINT32_MAX;
        XrXirCallCodeKind code=XR_XIR_CALL_CODE_UNKNOWN;bool invoked=false;
        XrXirCallStatus authenticated=xir_call_debug_frame_entry(view,parent->event.frame,&function,&code);
        if (authenticated==XR_XIR_CALL_BAD_STATE) break;
        if (authenticated!=XR_XIR_CALL_READY) { status=authenticated==XR_XIR_CALL_LIMIT ? XR_XIR_VALUE_LIMIT : XR_XIR_VALUE_BAD_ARGUMENT;break; }
        if (function!=parent->event.entry || code!=parent->event.code) { status=XR_XIR_VALUE_BAD_ARGUMENT;break; }
        if (code==XR_XIR_CALL_CODE_VM) {
            const DebugFrame *live=find_id(debug,parent->id);
            if (!live || live->point.binding->artifact!=debug->artifact || live->point.binding->function!=function ||
                !live->waiting || live->pending.instruction!=live->point.instruction) { status=XR_XIR_VALUE_BAD_ARGUMENT;break; }
            instruction=live->pending.instruction;callee=live->pending.callee;invoked=live->pending.invoked;
        } else if (code==XR_XIR_CALL_CODE_NATIVE) {
            if (!debug->native_code || function>=debug->native_code->count ||
                debug->native_code->offsets[function]==UINT32_MAX || !parent->native_waiting) break;
            instruction=parent->native_instruction;callee=parent->native_callee;invoked=parent->native_invoked;
        } else break;
        const XrXirModule *module=xr_xir_compile_artifact_module(debug->artifact);
        if (function>=module->function_count || instruction>=module->functions[function].instruction_count ||
            callee!=call->event.entry) { status=XR_XIR_VALUE_BAD_ARGUMENT;break; }
        XrXirOp op=module->functions[function].instructions[instruction].op;
        if (invoked!=(op==XR_XIR_INVOKE || op==XR_XIR_INVOKE_INDIRECT) ||
            (!invoked && op!=XR_XIR_CALL && op!=XR_XIR_CALL_INDIRECT)) { status=XR_XIR_VALUE_BAD_ARGUMENT;break; }
        /* The original callback owned the pending ordinal and resolved
         * callee. This real child ENTER now proves that proposal was admitted. */
        status=debug_exception_function(debug,view,(XrXirVmDebugLocation){parent->event.entry,instruction},error,&selection);
        if (status!=XR_XIR_VALUE_OK) break;
        call=parent;++ancestors;
    }
    if (status==XR_XIR_VALUE_OK) *output=result;
    return status;
}
