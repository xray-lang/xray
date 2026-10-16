/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_generator.inc.c - Independent generator transfer and value ownership
 *
 * KEY CONCEPT:
 *   Yield is a typed transfer; only scheduling suspension owns a wake request.
 */
static const XrXirTypeNode *generator_signature(const XrXirCallConfig *config,
    const XrXirCallEntry *entry) {
    if (!config || !entry || !(entry->flags & XR_XIR_ENTRY_GENERATOR)) return NULL;
    const XrXirTypeNode *node=xr_xir_type_node(
        xr_xir_compile_type_arena_types(config->admission.arena),entry->result);
    return node && node->kind==XR_XIR_TYPE_ITERATOR && !node->parameter_span ? node : NULL;
}
static void accept_generator_yield(XrXirCall *call,XrXirValue value) {
    if (cleanup_active(call) && xr_xir_value_valid(&value)) {
        XrXirCallResult result=call_result(XR_XIR_CALL_DEFER_ASYNC);
        result.panic.detail.code=XR_XIR_PANIC_DEFER_ASYNC;
        request_exit(call,&result,false);return;
    }
    const XrXirTypeNode *signature=generator_signature(&call->config,call->top->entry);
    if (!signature || call->top->parent) {abort_frames(call,XR_XIR_CALL_BAD_STATE);return;}
    call->admitting=true;
    XrXirCallStatus status=admit_value(&value,signature->element,&call->config.admission);
    call->admitting=false;
    if (status!=XR_XIR_CALL_READY) {
        if (status==XR_XIR_CALL_OOM || status==XR_XIR_CALL_LIMIT) request_execution_failure(call,status);
        else abort_frames(call,XR_XIR_CALL_BAD_STATE);
        return;
    }
    XrXirCallResult result=call_result(XR_XIR_CALL_YIELDED);
    XrXirValueStatus copied=xr_xir_value_copy(&value,&result.value);
    if (copied!=XR_XIR_VALUE_OK) {
        request_execution_failure(call,copied==XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM : XR_XIR_CALL_LIMIT);return;
    }
    call->result=result;
}
XR_FUNC XrXirCallStatus xr_xir_call_generator_new(const XrXirCallConfig *config,uint32_t entry,
    const XrXirValue *arguments,uint32_t count,XrXirCall **output) {
    if (!output || *output) return XR_XIR_CALL_BAD_ARGUMENT;
    if (config && config->abi_version==XR_XIR_CALL_ABI_VERSION &&
        config->struct_size==sizeof(*config) && config->admission.domain &&
        !xr_xir_domain_current(config->admission.domain)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCallRequest request={entry,arguments,count,NULL,NULL};
    return call_new(config,entry,&request,NULL,output,true);
}
XR_FUNC XrXirCallStatus xr_xir_call_generator_new_budgeted(const XrXirCallConfig *config,
    const XrXirCallRequest *request,XrXirCallBudget *budget,XrXirCall **output) {
    if (!output || *output || !request || !budget || !xr_xir_domain_current(budget->work_domain))
        return XR_XIR_CALL_BAD_ARGUMENT;
    return call_new(config,request->entry,request,budget,output,true);
}
XR_FUNC XrXirCallStatus xr_xir_call_generator_take(XrXirCall *call,XrXirValue *output) {
    if (!call_owner_current(call) || !call || !output || !boundary_value(*output,XR_XIR_UNIT))
        return XR_XIR_CALL_BAD_ARGUMENT;
    if (call->driving || call_observing(call)) return XR_XIR_CALL_BUSY;
    if (call->result.status!=XR_XIR_CALL_YIELDED || !call->top || call->top->parent ||
        !generator_signature(&call->config,call->top->entry)) return XR_XIR_CALL_BAD_STATE;
    *output=call->result.value;call->result=call_result(XR_XIR_CALL_READY);
    return XR_XIR_CALL_YIELDED;
}
