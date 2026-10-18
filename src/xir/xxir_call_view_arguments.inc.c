/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_view_arguments.inc.c - Prepared-frame temporary view borrowing
 *
 * KEY CONCEPT:
 *   Actual caller bindings authorize each new descendant before frame commit.
 */
typedef struct CallViewArguments {
    XrXirCall *call;
    CallFrame *source,*destination;
} CallViewArguments;
static bool call_view_arguments_current(const CallViewArguments *transfer,XrXirValueAdmission *admission) {
    return transfer && call_owner_current(transfer->call) && transfer->call->admitting &&
        transfer->call->active_view && transfer->source==transfer->call->top &&
        admission==xr_xir_call_admission(transfer->call->active_view) &&
        xr_xir_call_execution_status(transfer->call->active_view)==XR_XIR_CALL_READY;
}
static XrXirValueStatus call_view_argument_source(void *context,const XrXirValue *value,bool writable,
    XrXirValueAdmission *admission,XrXirCellAuthority *output) {
    CallViewArguments *transfer=context;(void)writable;
    if (!call_view_arguments_current(transfer,admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    CallViewBinding *binding=call_view_binding(transfer->call,value,&status);
    if (binding) *output=binding->pin.authority;
    return status;
}
static XrXirValueStatus call_view_argument_destination(void *context,const XrXirValue *value,bool writable,
    XrXirValueAdmission *admission,XrXirCellAuthority *output) {
    CallViewArguments *transfer=context;(void)writable;
    if (!call_view_arguments_current(transfer,admission) || !transfer->destination ||
        transfer->destination->parent!=transfer->source) return XR_XIR_VALUE_BAD_ARGUMENT;
    void *body=NULL;memcpy(&body,&value->payload,sizeof(body));
    for (CallViewBinding *binding=transfer->destination->views;binding;binding=binding->next) {
        if (!call_cell_work(transfer->call,1)) return XR_XIR_VALUE_LIMIT;
        if (binding->pin.body==body) {*output=binding->pin.authority;return XR_XIR_VALUE_OK;}
    }
    return XR_XIR_VALUE_BAD_ARGUMENT;
}
static XrXirCallStatus call_view_argument_admit(XrXirCall *call,const XrXirValue *value,XrXirType type) {
    if (!call || !call->top) return XR_XIR_CALL_BAD_ARGUMENT;
    CallViewArguments transfer={call,call->top,NULL};
    XrXirViewAccess access={&call->config.admission,call_view_argument_source,&transfer};
    XrXirValueStatus status=xr_xir_borrowed_value_admit(value,type,&access);
    return status==XR_XIR_VALUE_OK ? XR_XIR_CALL_READY : status==XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM :
        status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_BAD_ARGUMENT;
}
static XrXirValueStatus call_view_argument_leaf(void *context,const XrXirValue *source,XrXirValue *output) {
    CallViewArguments *transfer=context;XrXirCall *call=transfer->call;
    XrXirViewAccess access={&call->config.admission,call_view_argument_source,transfer};
    int64_t length=0;XrXirValueStatus status=xr_xir_view_len(source,&access,&length);
    if (status!=XR_XIR_VALUE_OK) return status;
    CallViewBinding *parent=call_view_binding(call,source,&status);if (!parent) return status;
    const XrXirTypeNode *node=xr_xir_type_node(xr_xir_compile_type_arena_types(access.values->arena),(XrXirType)source->type);
    if (!node) return XR_XIR_VALUE_BAD_ARGUMENT;
    CallViewCreation creation={.call=call,.frame=transfer->destination,.root_loan=parent->root_loan,
        .spec={.kind=node->kind,.length=length,.authority=parent->pin.authority}};
    status=call_view_allocate(&creation);if (status!=XR_XIR_VALUE_OK) return status;
    status=xr_xir_view_reborrow((XrXirType)source->type,source,&creation.spec,&access,output);
    if (status==XR_XIR_VALUE_OK) {
        xr_xir_view_scope_pin_created(output,&creation.spec.authority,&creation.binding->pin);
        creation.binding->next=transfer->destination->views;transfer->destination->views=creation.binding;
    } else call_deallocate(call->config.accounting,call->budget,creation.binding,sizeof(*creation.binding));
    return status;
}
static XrXirCallStatus call_view_argument_copy(XrXirCall *call,CallFrame *frame,
    const XrXirValue *value,XrXirType type,XrXirValue *output) {
    XrXirValueStatus status;
    if (xr_xir_compile_type_arena_borrowed(call->config.admission.arena,type)) {
        CallViewArguments context={call,call->top,frame};
        XrXirViewAccess source={&call->config.admission,call_view_argument_source,&context};
        XrXirViewAccess destination={&call->config.admission,call_view_argument_destination,&context};
        const XrXirViewTransfer transfer={&source,&destination,call_view_argument_leaf,&context};
        call->admitting=true;status=xr_xir_borrowed_value_reborrow(value,type,&transfer,output);call->admitting=false;
    } else status=xr_xir_value_copy(value,output);
    return status==XR_XIR_VALUE_OK ? XR_XIR_CALL_READY : status==XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM :
        status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_BAD_ARGUMENT;
}
static void frame_view_arguments_release(XrXirCall *call,CallFrame *frame) {
    bool admitting=call->admitting;call->admitting=true;
    frame_view_release(call,frame);
    for (uint32_t p=frame->entry->parameter_count;p;--p) xr_xir_value_drop(&frame->arguments[p-1]);
    if (frame->view_return) xr_xir_view_origin_unpin(&frame->view_return->origin);
    call->admitting=admitting;
}
XR_FUNC XrXirValueStatus xr_xir_call_view_argument_take(XrXirCallView *view,
    uint32_t parameter,XrXirValue *output) {
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if (!admission || view->activation->admitting || view->activation->top->return_staged || view->phase!=XR_XIR_CALL_NORMAL ||
        xr_xir_call_execution_status(view)!=XR_XIR_CALL_READY || parameter>=view->argument_count ||
        !call_view_slot(view->activation->top,output) || !output || output->type || output->reserved || output->payload)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirCall *call=view->activation;CallFrame *frame=call->top;XrXirType type=frame->entry->parameters[parameter];
    if (!xr_xir_compile_type_arena_borrowed(admission->arena,type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    call->admitting=true;XrXirCallStatus status=call_view_argument_admit(call,&frame->arguments[parameter],type);
    if (status==XR_XIR_CALL_READY && !call_cell_work(call,2*sizeof(*output))) status=XR_XIR_CALL_LIMIT;
    if (status==XR_XIR_CALL_READY) {*output=frame->arguments[parameter];frame->arguments[parameter]=(XrXirValue){0};}
    call->admitting=false;
    return status==XR_XIR_CALL_READY ? XR_XIR_VALUE_OK : call_view_status(status);
}
