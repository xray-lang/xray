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

static bool iterator_wait_kind(uint32_t kind) {
    return kind==XR_XIR_WAIT_ITERATOR_HAS_NEXT || kind==XR_XIR_WAIT_ITERATOR_NEXT;
}
static void accept_iterator_wait(XrXirCall *call,XrXirAction action) {
    const XrXirTypeNode *type=xr_xir_type_node(
        xr_xir_compile_type_arena_types(call->config.admission.arena),(XrXirType)action.value.type);
    if(!iterator_wait_kind(action.callee) || action.arguments || action.argument_count ||
        !call->executor_owner || !call->executor_generation || !call->executor_ticket ||
        !type || type->kind!=XR_XIR_TYPE_ITERATOR ||
        !xr_xir_value_argument(&action.value,call->config.admission.arena,(XrXirType)action.value.type) ||
        call->next_wake==UINT64_MAX) {abort_frames(call,XR_XIR_CALL_BAD_STATE);return;}
    if(cleanup_active(call)) {
        XrXirCallResult result=call_result(XR_XIR_CALL_DEFER_ASYNC);
        result.panic.detail.code=XR_XIR_PANIC_DEFER_ASYNC;request_exit(call,&result,false);return;
    }
    const XirGenerator *generator=(const XirGenerator *)(uintptr_t)action.value.payload;
    XirGeneratorState state=atomic_load_explicit(&generator->state,memory_order_acquire);
    if(generator->object.domain!=call->config.admission.domain ||
        generator->generation!=call->executor_generation || !generator->identity ||
        (state!=XIR_GENERATOR_TERMINAL && generator->executor!=call->executor_owner)) {
        abort_frames(call,XR_XIR_CALL_BAD_STATE);return;
    }
    if(generator->identity==call->executor_ticket) {
        /* An authentic self-demand fails execution with intact cleanup state. */
        request_execution_failure(call,XR_XIR_CALL_BAD_STATE);return;
    }
    XrXirValue prepared={0};XrXirValueStatus copied=xr_xir_value_copy(&action.value,&prepared);
    if(copied!=XR_XIR_VALUE_OK) {
        if(copied==XR_XIR_VALUE_OOM || copied==XR_XIR_VALUE_LIMIT || copied==XR_XIR_VALUE_REFCOUNT_LIMIT)
            request_execution_failure(call,copied==XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM : XR_XIR_CALL_LIMIT);
        else abort_frames(call,XR_XIR_CALL_BAD_STATE);
        return;
    }
    if(call->cancel_requested) {xr_xir_value_drop(&prepared);request_exit_status(call,XR_XIR_CALL_CANCELLED);return;}
    XR_CHECK(!call->waiting_task.type,"one activation owns one pending execution dependency");
    call->waiting_task=prepared;call->top->protected_call=(action.flags & XR_XIR_ACTION_PROTECTED)!=0;
    call->result=call_result(XR_XIR_CALL_SUSPENDED);call->result.wake=++call->next_wake;
    call->wait=(XrXirWaitRequest){action.callee,0,0,generator,generator->generation,generator->identity};
}
