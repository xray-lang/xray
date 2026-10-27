/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_view.inc.c - Actual frame binding and prepaid terminal view revocation
 *
 * KEY CONCEPT:
 *   Physical pins stabilize exact registered identities. Only the current real
 *   callback can access them, and every frame pop revokes permission first.
 */
#include "xxir_instance_view_internal.h"
struct CallViewBinding {
    CallViewBinding *next;
    XrXirViewOriginPin pin;
    const XrXirCellLoan *root_loan;
    bool returned;
};
static XrXirValueStatus call_view_status(XrXirCallStatus status) {
    return status==XR_XIR_CALL_OOM ? XR_XIR_VALUE_OOM : status==XR_XIR_CALL_LIMIT ?
        XR_XIR_VALUE_LIMIT : XR_XIR_VALUE_BAD_ARGUMENT;
}
static bool call_view_slot(const CallFrame *frame,const XrXirValue *value) {
    uintptr_t start=(uintptr_t)frame->state,address=(uintptr_t)value;
    return value && frame->entry->state_bytes>=sizeof(*value) && address>=start &&
        address-start<=frame->entry->state_bytes-sizeof(*value) && !(address%_Alignof(XrXirValue));
}
static CallViewBinding *call_view_binding(XrXirCall *call,const XrXirValue *value,XrXirValueStatus *status) {
    void *body=NULL;memcpy(&body,&value->payload,sizeof(body));
    for (CallViewBinding *binding=call->top->views;binding;binding=binding->next) {
        if (!call_cell_work(call,1)) {*status=XR_XIR_VALUE_LIMIT;return NULL;}
        if (binding->pin.body==body) return binding;
    }
    *status=XR_XIR_VALUE_BAD_ARGUMENT;return NULL;
}
static XrXirValueStatus call_view_authorize(void *context,const XrXirValue *value,bool writable,
    XrXirValueAdmission *admission,XrXirCellAuthority *output) {
    XrXirCall *call=context;(void)writable;
    if (!call_owner_current(call) || !call || !call->active_view || call->admitting ||
        admission!=xr_xir_call_admission(call->active_view) ||
        xr_xir_call_execution_status(call->active_view)!=XR_XIR_CALL_READY) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status=XR_XIR_VALUE_OK;CallViewBinding *binding=call_view_binding(call,value,&status);
    if (binding) *output=binding->pin.authority;
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_call_view_access(const XrXirCallView *view,XrXirViewAccess *output) {
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if (!admission || !admission->domain || !output || output->values || output->authorize || output->context ||
        view->activation->admitting || xr_xir_call_execution_status(view)!=XR_XIR_CALL_READY)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    *output=(XrXirViewAccess){admission,call_view_authorize,view->activation};return XR_XIR_VALUE_OK;
}
typedef struct CallViewCreation {
    XrXirCall *call;
    CallFrame *frame;
    CallViewBinding *binding;
    XrXirViewStorageSpec spec;
    const XrXirValue *parent;
    XrXirCellAuthority parent_authority;
    const XrXirCellLoan *root_loan;
} CallViewCreation;
static XrXirValueStatus call_view_parent_authorize(void *context,const XrXirValue *value,bool writable,
    XrXirValueAdmission *admission,XrXirCellAuthority *output) {
    CallViewCreation *c=context;(void)writable;
    if (value!=c->parent || !c->call->admitting || c->call->top!=c->frame ||
        !c->call->active_view || admission!=xr_xir_call_admission(c->call->active_view))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    *output=c->parent_authority;return XR_XIR_VALUE_OK;
}
static XrXirValueStatus call_view_prepare(XrXirCallView *view,XrXirType type,
    XrXirValue *output,CallViewCreation *creation) {
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if (!admission || !admission->domain || view->phase!=XR_XIR_CALL_NORMAL || view->activation->admitting ||
        xr_xir_call_execution_status(view)!=XR_XIR_CALL_READY ||
        view->activation->top->return_staged ||
        !call_view_slot(view->activation->top,output) || output->type || output->reserved || output->payload)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypeNode *node=xr_xir_type_node(xr_xir_compile_type_arena_types(admission->arena),type);
    if (!node || (node->kind!=XR_XIR_TYPE_SLICE && node->kind!=XR_XIR_TYPE_MUT_SLICE)) return XR_XIR_VALUE_BAD_ARGUMENT;
    creation->call=view->activation;creation->frame=view->activation->top;
    creation->spec.kind=node->kind;return XR_XIR_VALUE_OK;
}
static XrXirValueStatus call_view_collect(XrXirCall *call,bool charge) {
    CallViewBinding **link=&call->top->views;
    while (*link) {
        if (charge && !call_cell_work(call,1)) return XR_XIR_VALUE_LIMIT;
        CallViewBinding *binding=*link;
        if (xr_xir_view_scope_active(&binding->pin)) {link=&binding->next;continue;}
        *link=binding->next;xr_xir_view_scope_close(&binding->pin);
        call_deallocate(call->config.accounting,call->budget,binding,sizeof(*binding));
    }
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_call_view_drop(XrXirCallView *view,XrXirValue *value) {
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if (!admission || view->activation->admitting || view->activation->top->return_staged || xr_xir_call_execution_status(view)!=XR_XIR_CALL_READY ||
        !call_view_slot(view->activation->top,value) || !xr_xir_value_valid(value) ||
        !xr_xir_compile_type_arena_borrowed(admission->arena,(XrXirType)value->type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirCall *call=view->activation;
    for (CallViewBinding *binding=call->top->views;binding;binding=binding->next)
        if (!call_cell_work(call,1)) return XR_XIR_VALUE_LIMIT;
    call->admitting=true;xr_xir_value_drop(value);
    XrXirValueStatus status=call_view_collect(call,false);
    call->admitting=false;
    XR_CHECK(status==XR_XIR_VALUE_OK,"prepaid binding retirement cannot fail");return status;
}
static XrXirValueStatus call_view_allocate(CallViewCreation *c) {
    /* The pin, registry write, terminal traversal and revocation never need
     * a fresh allocation or quota after the view constructor commits its COW. */
    if (!call_cell_work(c->call,sizeof(CallViewBinding)+sizeof(XrXirViewOriginPin)+2*sizeof(void *)+16))
        return XR_XIR_VALUE_LIMIT;
    XrXirCallStatus status=XR_XIR_CALL_READY;
    c->binding=call_allocate(&c->call->config,c->call->budget,sizeof(*c->binding),&status);
    if (!c->binding) return call_view_status(status);
    *c->binding=(CallViewBinding){0};
    c->binding->root_loan=c->root_loan;
    /* A stable binding remains distinct when an interpreter reuses one owned
     * scratch slot for several simultaneously live mutable view results. */
    if (c->spec.kind==XR_XIR_TYPE_MUT_SLICE)
        c->spec.authority=(XrXirCellAuthority){c->call,c->binding,c->frame->epoch};
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus call_view_reserve(CallViewCreation *c) {
    c->call->admitting=true;
    XrXirValueStatus collected=call_view_collect(c->call,true);
    c->call->admitting=false;
    return collected==XR_XIR_VALUE_OK ? call_view_allocate(c) : collected;
}
static void call_view_finish(CallViewCreation *c,XrXirValueStatus status,XrXirValue *created,XrXirValue *output) {
    if (status==XR_XIR_VALUE_OK) {
        xr_xir_view_scope_pin_created(created,&c->spec.authority,&c->binding->pin);
        c->binding->next=c->frame->views;c->frame->views=c->binding;
        *output=*created;*created=(XrXirValue){0};
    } else call_deallocate(c->call->config.accounting,c->call->budget,c->binding,sizeof(*c->binding));
    c->call->admitting=false;
}
XR_FUNC XrXirValueStatus xr_xir_call_view_new(XrXirCallView *view,XrXirType type,
    uint32_t parameter,int64_t offset,int64_t length,XrXirValue *output) {
    CallViewCreation c={0};XrXirValueStatus status=call_view_prepare(view,type,output,&c);
    if (status!=XR_XIR_VALUE_OK) return status;
    if (parameter>=c.frame->entry->parameter_count || offset<0 || length<0) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirCellLoan *root=NULL;
    for (uint32_t i=0;i<c.frame->loan_count;++i) {
        if (!call_cell_work(c.call,1)) return XR_XIR_VALUE_LIMIT;
        if (c.frame->loans[i].parameter==parameter) root=&c.frame->loans[i].loan;
    }
    if (!root || xr_xir_cell_is_projection(&c.frame->arguments[parameter])) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypes *types=xr_xir_compile_type_arena_types(c.call->config.admission.arena);
    const XrXirTypeNode *cell=xr_xir_type_node(types,c.frame->entry->parameters[parameter]);
    const XrXirTypeNode *result=xr_xir_type_node(types,type);
    const XrXirTypeNode *array=cell ? xr_xir_type_node(types,cell->element) : NULL;
    bool string=cell && cell->kind==XR_XIR_TYPE_CELL && cell->element==XR_XIR_STRING;
    if (!cell || cell->kind!=XR_XIR_TYPE_CELL ||
        (string ? c.spec.kind!=XR_XIR_TYPE_SLICE || result->element!=XR_XIR_U8 :
         !array || array->kind!=XR_XIR_TYPE_ARRAY || result->element!=array->element)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirCellAuthority parent=frame_cell_authority(c.call,c.frame);
    c.root_loan=root;
    c.spec.authority=c.spec.kind==XR_XIR_TYPE_SLICE ? parent : (XrXirCellAuthority){c.call,output,c.frame->epoch};
    c.spec.offset=offset;c.spec.length=length;status=call_view_reserve(&c);
    if (status!=XR_XIR_VALUE_OK) return status;
    XrXirValue created={0};c.call->admitting=true;
    status=xr_xir_view_new(type,&c.frame->arguments[parameter],&parent,&c.spec,&c.call->config.admission,&created);
    call_view_finish(&c,status,&created,output);return status;
}
XR_FUNC XrXirValueStatus xr_xir_call_view_reborrow(XrXirCallView *view,XrXirType type,
    const XrXirValue *parent,int64_t offset,int64_t length,XrXirValue *output) {
    CallViewCreation c={0};XrXirValueStatus status=call_view_prepare(view,type,output,&c);
    if (status!=XR_XIR_VALUE_OK) return status;
    if (!xr_xir_value_valid(parent) || offset<0 || length<0) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypeNode *source_type=xr_xir_type_node(xr_xir_compile_type_arena_types(c.call->config.admission.arena),
        (XrXirType)parent->type);
    if (!source_type || (c.spec.kind==XR_XIR_TYPE_MUT_SLICE && source_type->kind!=XR_XIR_TYPE_MUT_SLICE))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    CallViewBinding *source=call_view_binding(c.call,parent,&status);if (!source) return status;
    c.root_loan=source->root_loan;
    if (!xr_xir_view_scope_active(&source->pin)) return XR_XIR_VALUE_BAD_ARGUMENT;
    c.spec.authority=c.spec.kind==XR_XIR_TYPE_SLICE ? source->pin.authority :
        (XrXirCellAuthority){c.call,output,c.frame->epoch};
    c.parent=parent;c.parent_authority=source->pin.authority;
    c.spec.offset=offset;c.spec.length=length;status=call_view_reserve(&c);
    if (status!=XR_XIR_VALUE_OK) return status;
    XrXirValue created={0};XrXirViewAccess access={&c.call->config.admission,call_view_parent_authorize,&c};
    c.call->admitting=true;
    status=xr_xir_view_reborrow(type,parent,&c.spec,&access,&created);
    call_view_finish(&c,status,&created,output);return status;
}
static XrXirValueStatus call_view_storage(CallViewCreation *c,XrXirType type,
    const XrXirValue *root,int64_t offset,int64_t length,XrXirValue *output) {
    const XrXirTypes *types=xr_xir_compile_type_arena_types(c->call->config.admission.arena);
    const XrXirTypeNode *cell=xr_xir_type_node(types,(XrXirType)root->type);
    const XrXirTypeNode *result=xr_xir_type_node(types,type);
    const XrXirTypeNode *array=cell ? xr_xir_type_node(types,cell->element) : NULL;
    bool string=cell && cell->kind==XR_XIR_TYPE_CELL && cell->element==XR_XIR_STRING;
    if (!cell || cell->kind!=XR_XIR_TYPE_CELL ||
        (string ? c->spec.kind!=XR_XIR_TYPE_SLICE || result->element!=XR_XIR_U8 :
         !array || array->kind!=XR_XIR_TYPE_ARRAY || result->element!=array->element)) return XR_XIR_VALUE_BAD_ARGUMENT;
    c->spec.authority=c->spec.kind==XR_XIR_TYPE_SLICE ? frame_cell_authority(c->call,c->frame) :
        (XrXirCellAuthority){c->call,output,c->frame->epoch};
    c->spec.offset=offset;c->spec.length=length;XrXirValueStatus status=call_view_reserve(c);
    if (status!=XR_XIR_VALUE_OK) return status;
    XrXirValue created={0};c->call->admitting=true;
    status=xr_xir_view_new(type,root,NULL,&c->spec,&c->call->config.admission,&created);
    call_view_finish(c,status,&created,output);return status;
}
XR_FUNC XrXirValueStatus xr_xir_call_view_local(XrXirCallView *view,XrXirType type,
    const XrXirValue *root,int64_t offset,int64_t length,XrXirValue *output) {
    CallViewCreation c={0};XrXirValueStatus status=call_view_prepare(view,type,output,&c);
    if (status!=XR_XIR_VALUE_OK) return status;
    if (!call_view_slot(c.frame,root) || !xr_xir_value_valid(root) ||
        xr_xir_cell_is_projection(root) || xr_xir_cell_module_storage(root) ||
        offset<0 || length<0) return XR_XIR_VALUE_BAD_ARGUMENT;
    return call_view_storage(&c,type,root,offset,length,output);
}
static XrXirValueStatus call_view_module(XrXirCallView *view,XrXirType type,
    const XrXirValue *root,int64_t offset,int64_t length,XrXirValue *output) {
    CallViewCreation c={0};XrXirValueStatus status=call_view_prepare(view,type,output,&c);
    if (status!=XR_XIR_VALUE_OK) return status;
    if (c.spec.kind!=XR_XIR_TYPE_SLICE || !call_view_slot(c.frame,root) || offset<0 || length<0)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirCallStatus admitted=xir_instance_view_module_root(view,root);
    return admitted==XR_XIR_CALL_READY ? call_view_storage(&c,type,root,offset,length,output) : call_view_status(admitted);
}
static void frame_view_release(XrXirCall *call,CallFrame *frame) {
    while (frame->views) {
        CallViewBinding *binding=frame->views;frame->views=binding->next;
        xr_xir_view_scope_close(&binding->pin);
        call_deallocate(call->config.accounting,call->budget,binding,sizeof(*binding));
    }
}
