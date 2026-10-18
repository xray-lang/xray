/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_view_returns.inc.c - Signature-selected returned leaf adoption
 *
 * KEY CONCEPT:
 *   Every fallible origin and root check precedes return ownership mutation.
 */
static XrXirCallStatus call_view_return_capture(XrXirCall *call,CallFrame *frame,
    const XrXirValue *arguments,uint32_t captures) {
    CallViewReturn *result=frame->view_return;if (!result) return XR_XIR_CALL_READY;
    if (!frame->parent || !call->active_view) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirTypes *types=xr_xir_compile_type_arena_types(call->config.admission.arena);
    uint32_t count=0,permission=0;
    for (uint32_t p=captures;p<frame->entry->parameter_count;++p) {
        if (!call_cell_work(call,1)) return XR_XIR_CALL_LIMIT;
        XrXirType type=frame->entry->parameters[p];bool reference=xr_xir_type_is_cell(types,type);
        uint32_t leaves=reference ? 1u : xr_xir_compile_type_arena_borrowed_leaves(call->config.admission.arena,type);
        if (!leaves) continue;
        count=count>1 || leaves>1 ? 2u : count+leaves;result->parameter=p;result->reference=reference;
        permission=reference ? XR_XIR_VIEW_MUTABLE : xr_xir_compile_type_arena_borrowed(call->config.admission.arena,type);
    }
    uint32_t flags=xr_xir_compile_type_arena_borrowed(call->config.admission.arena,frame->entry->result);
    if (count!=1 || ((flags&XR_XIR_VIEW_MUTABLE) && !(permission&XR_XIR_VIEW_MUTABLE))) return XR_XIR_CALL_BAD_ARGUMENT;
    if (result->reference) return XR_XIR_CALL_READY;
    CallViewArguments context={call,frame->parent,NULL};
    XrXirViewAccess access={&call->config.admission,call_view_argument_source,&context};
    call->admitting=true;
    XrXirValueStatus status=xr_xir_borrowed_value_origin_pin(arguments+result->parameter-captures,
        frame->entry->parameters[result->parameter],&access,&result->origin);
    call->admitting=false;result->authority=result->origin.authority;
    return status==XR_XIR_VALUE_OK ? XR_XIR_CALL_READY : status==XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM :
        status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_BAD_ARGUMENT;
}
static XrXirCallStatus call_view_return_bind(XrXirCall *call,CallFrame *frame) {
    CallViewReturn *result=frame->view_return;
    if (!result || !result->reference) return XR_XIR_CALL_READY;
    for (uint32_t i=0;i<frame->loan_count;++i) {
        if (!call_cell_work(call,1)) return XR_XIR_CALL_LIMIT;
        if (frame->loans[i].parameter==result->parameter) {result->loan=&frame->loans[i];return XR_XIR_CALL_READY;}
    }
    return XR_XIR_CALL_BAD_ARGUMENT;
}
static XrXirValueStatus call_view_return_leaf(void *context,const XrXirValue *value,bool writable,
    XrXirValueAdmission *admission,XrXirCellAuthority *output) {
    CallViewArguments *current=context;(void)writable;
    if (!call_view_arguments_current(current,admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status=XR_XIR_VALUE_OK;CallViewBinding *binding=call_view_binding(current->call,value,&status);
    if (!binding) return status;
    CallViewReturn *result=current->source->view_return;
    if (result->reference) {
        if (binding->root_loan!=&result->loan->loan) return XR_XIR_VALUE_BAD_ARGUMENT;
        XrXirViewOriginInfo info={0};status=xr_xir_view_origin_info(&binding->pin,admission,&info);
        if (status!=XR_XIR_VALUE_OK) return status;
        if (!info.logical_active || !xr_xir_cell_same_owner(info.root,&current->source->arguments[result->parameter]))
            return XR_XIR_VALUE_BAD_ARGUMENT;
    }
    binding->returned=true;*output=binding->pin.authority;return XR_XIR_VALUE_OK;
}
static XrXirValueStatus call_view_return_root_prepare(XrXirCall *call,CallFrame *frame) {
    CallViewReturn *result=frame->view_return;
    if (!result->reference) return XR_XIR_VALUE_OK;
    result->parent_loan=result->loan->loan.previous;
    if (result->parent_loan) {
        bool found=false;
        for (uint32_t i=0;i<frame->parent->loan_count;++i) {
            if (!call_cell_work(call,1)) return XR_XIR_VALUE_LIMIT;
            found|=result->parent_loan==&frame->parent->loans[i].loan && !frame->parent->loans[i].adopted;
        }
        if (!found) return XR_XIR_VALUE_BAD_ARGUMENT;
    }
    result->authority=frame_cell_authority(call,frame->parent);
    XrXirCellAuthority authority=frame_cell_authority(call,frame);
    return xr_xir_view_ref_return_prepare(&frame->arguments[result->parameter],&result->loan->loan,
        &authority,&call->config.admission);
}
static XrXirCallStatus call_view_result_admit(XrXirCall *call,const XrXirValue *value,XrXirType type) {
    XrXirValueAdmission *admission=&call->config.admission;CallFrame *frame=call->top;
    if (!xr_xir_compile_type_arena_borrowed(admission->arena,type)) return admit_value(value,type,admission);
    if (!frame || !frame->view_return || type!=frame->entry->result || !frame->parent || !call->admitting)
        return XR_XIR_CALL_BAD_ARGUMENT;
    CallViewReturn *result=frame->view_return;result->prepared=false;
    for (CallViewBinding *binding=frame->views;binding;binding=binding->next) {
        if (!call_cell_work(call,sizeof(*binding)+sizeof(XrXirCellAuthority)+4u*sizeof(void *))) return XR_XIR_CALL_LIMIT;
        binding->returned=false;
    }
    CallViewArguments current={call,frame,NULL};XrXirViewAccess access={admission,call_view_return_leaf,&current};
    XrXirValueStatus status=result->reference ? xr_xir_borrowed_value_admit(value,type,&access) :
        xr_xir_borrowed_value_origin_match(value,type,&result->origin,&access);
    bool any=false;
    for (CallViewBinding *binding=frame->views;binding;binding=binding->next) any|=binding->returned;
    if (status==XR_XIR_VALUE_OK && any) status=call_view_return_root_prepare(call,frame);
    if (status==XR_XIR_VALUE_OK) result->prepared=true;
    else for (CallViewBinding *binding=frame->views;binding;binding=binding->next) binding->returned=false;
    return status==XR_XIR_VALUE_OK ? XR_XIR_CALL_READY : status==XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM :
        status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_BAD_ARGUMENT;
}
static void call_view_return_adopt(XrXirCall *call,CallFrame *frame) {
    CallViewReturn *result=frame->view_return;bool any=false;
    XR_CHECK(result && result->prepared && frame->parent,"terminal adoption owns a complete actual caller proof");
    bool admitting=call->admitting;call->admitting=true;
    CallViewBinding **link=&frame->views;
    while (*link) {
        CallViewBinding *binding=*link;
        if (binding->returned) {link=&binding->next;continue;}
        *link=binding->next;xr_xir_view_scope_close(&binding->pin);
        call_deallocate(call->config.accounting,call->budget,binding,sizeof(*binding));
    }
    while (frame->views) {
        CallViewBinding *binding=frame->views;frame->views=binding->next;
        xr_xir_view_scope_adopt(&binding->pin,&result->authority);
        if (result->reference) binding->root_loan=result->parent_loan;
        binding->returned=false;binding->next=frame->parent->views;frame->parent->views=binding;any=true;
    }
    if (any && result->reference) {
        xr_xir_view_ref_return_commit(&result->loan->loan);result->loan->adopted=true;
    }
    result->prepared=false;call->admitting=admitting;
}
XR_FUNC XrXirValueStatus xr_xir_call_view_inbox_take(XrXirCallView *view,
    XrXirType type,XrXirValue *output) {
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if (!admission || view->activation->admitting || view->phase!=XR_XIR_CALL_NORMAL ||
        xr_xir_call_execution_status(view)!=XR_XIR_CALL_READY || view->activation->top->return_staged ||
        !call_view_slot(view->activation->top,output) || !boundary_value(*output,XR_XIR_UNIT) ||
        !xr_xir_compile_type_arena_borrowed(admission->arena,type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirCall *call=view->activation;CallFrame *frame=call->top;XrXirCallResult *owner=&frame->inbox;
    if (owner->status!=XR_XIR_CALL_RETURNED || view->inbox.status!=owner->status || owner->wake || view->inbox.wake ||
        !xr_xir_panic_empty(&owner->panic) || !xr_xir_panic_empty(&view->inbox.panic) ||
        owner->value.type!=view->inbox.value.type || owner->value.reserved!=view->inbox.value.reserved ||
        owner->value.payload!=view->inbox.value.payload) return XR_XIR_VALUE_BAD_ARGUMENT;
    call->admitting=true;XrXirCallStatus status=call_view_argument_admit(call,&owner->value,type);
    if (status==XR_XIR_CALL_READY && !call_cell_work(call,sizeof(*owner)+2u*sizeof(*output))) status=XR_XIR_CALL_LIMIT;
    if (status==XR_XIR_CALL_READY) {*output=owner->value;*owner=call_result(XR_XIR_CALL_READY);view->inbox=*owner;}
    call->admitting=false;
    return status==XR_XIR_CALL_READY ? XR_XIR_VALUE_OK : call_view_status(status);
}
static XrXirCallStatus call_view_inbox_discard(XrXirCallView *view) {
    XrXirCall *call=view->activation;
    if (call->admitting || call->top->return_staged) return XR_XIR_CALL_BAD_STATE;
    for (CallViewBinding *binding=call->top->views;binding;binding=binding->next)
        if (!call_cell_work(call,1)) return XR_XIR_CALL_LIMIT;
    call->admitting=true;xr_xir_call_result_drop(&call->top->inbox);view->inbox=call_result(XR_XIR_CALL_READY);
    XrXirValueStatus status=call_view_collect(call,false);call->admitting=false;
    XR_CHECK(status==XR_XIR_VALUE_OK,"prepaid discarded-result scope reclamation cannot fail");
    return XR_XIR_CALL_READY;
}
