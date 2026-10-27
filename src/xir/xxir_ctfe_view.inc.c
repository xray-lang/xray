/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_view.inc.c - Scoped readonly byte views in compiler activations
 *
 * KEY CONCEPT:
 *   Physical pins never grant access. The current compiler activation supplies
 *   exact authority and revokes every scope before releasing its Cell loans.
 */
#include "xxir_view_value_internal.h"

typedef struct CtfeViewBinding {
    struct CtfeViewBinding *next;
    XrXirViewOriginPin pin;
    XrXirType type;
} CtfeViewBinding;
typedef struct CtfeViewAccessContext {
    CtfeCallExecution *execution;
    CtfeCallFrame *frame;
    XrXirValueAdmission *admission;
    XrXirCtfeStatus failure;
} CtfeViewAccessContext;

static bool ctfe_view_current(const CtfeViewAccessContext *context) {
    const CtfeCallExecution *execution=context->execution;
    const CtfeCallFrame *frame=context->frame;
    if (!execution || !frame || execution->top!=frame || frame->execution!=execution ||
        execution->owned.activation!=frame || !frame->epoch || frame->state.leaving) return false;
    const XrXirCellAuthority *authority=&execution->owned.authority;
    return authority->activation==execution && authority->frame==frame && authority->epoch==frame->epoch &&
        context->admission && context->admission->domain==execution->owned.domain &&
        context->admission->arena==execution->owned.arena && xr_xir_domain_current(context->admission->domain) &&
        xr_xir_domain_is_compile(context->admission->domain);
}
static CtfeViewBinding *ctfe_view_find(CtfeViewAccessContext *context,const XrXirValue *value) {
    if (!ctfe_view_current(context) || !value || value->reserved) return NULL;
    void *body=NULL;memcpy(&body,&value->payload,sizeof(body));
    uint32_t at=0;
    for (CtfeViewBinding *binding=context->frame->views;binding;binding=binding->next) {
        if (!(at++ & 63u)) {
            context->failure=ctfe_control_poll(context->execution->owned.control);
            if (context->failure!=XR_XIR_CTFE_OK) return NULL;
        }
        if (binding->pin.body==body && binding->type==(XrXirType)value->type &&
            xr_xir_view_scope_active(&binding->pin)) return binding;
    }
    return NULL;
}
static XrXirValueStatus ctfe_view_authorize(void *opaque,const XrXirValue *value,bool writable,
    XrXirValueAdmission *admission,XrXirCellAuthority *output) {
    CtfeViewAccessContext *context=opaque;
    if (writable || admission!=context->admission) return XR_XIR_VALUE_BAD_ARGUMENT;
    CtfeViewBinding *binding=ctfe_view_find(context,value);
    if (!binding) return XR_XIR_VALUE_BAD_ARGUMENT;
    *output=binding->pin.authority;return XR_XIR_VALUE_OK;
}
static void ctfe_view_binding_free(CtfeCallFrame *frame,CtfeViewBinding *binding) {
    xr_xir_view_scope_close(&binding->pin);
    XR_CHECK(frame->execution->loan_bytes>=sizeof(*binding),"compiler view bookkeeping underflow");
    frame->execution->loan_bytes-=sizeof(*binding);
    xr_compile_resources_free(binding);
}
static void ctfe_view_collect(CtfeCallFrame *frame) {
    CtfeViewBinding **link=&frame->views;
    while (*link) {
        CtfeViewBinding *binding=*link;
        if (xr_xir_view_scope_active(&binding->pin)) {link=&binding->next;continue;}
        *link=binding->next;ctfe_view_binding_free(frame,binding);
    }
}
static void ctfe_view_frame_close(CtfeCallFrame *frame) {
    while (frame->views) {
        CtfeViewBinding *binding=frame->views;frame->views=binding->next;
        ctfe_view_binding_free(frame,binding);
    }
}
static XrXirCtfeStatus ctfe_view_reserve(CtfeViewAccessContext *context,CtfeViewBinding **output) {
    if (!ctfe_view_current(context)) return XR_XIR_CTFE_BAD_ARTIFACT;
    CtfeOwnedState *state=&context->execution->owned;
    if (sizeof(CtfeViewBinding)>ctfe_frame_spare(state)) return XR_XIR_CTFE_FRAME_LIMIT;
    if (context->execution->loan_bytes>UINT64_MAX-sizeof(CtfeViewBinding)) return XR_XIR_CTFE_OVERFLOW;
    void *memory=NULL;
    XrXirCtfeStatus status=ctfe_compile_status(xir_compile_resource_status(
        xr_compile_resources_calloc(state->context->resources,1,sizeof(CtfeViewBinding),&memory)));
    if (status!=XR_XIR_CTFE_OK) return status;
    context->execution->loan_bytes+=sizeof(CtfeViewBinding);*output=memory;
    context->admission->scratch_bytes=ctfe_frame_spare(state);
    return XR_XIR_CTFE_OK;
}
static XrXirCtfeStatus ctfe_view_root_length(CtfeViewAccessContext *context,
    const XrXirValue *root,int64_t *length) {
    CtfeOwnedState *state=&context->execution->owned;
    if (!xr_xir_cell_in_domain(root,state->domain) || xr_xir_cell_module_storage(root) ||
        xr_xir_cell_is_projection(root)) return XR_XIR_CTFE_NOT_SUPPORTED;
    const XrXirTypes *types=context->frame->run.module->types;
    XrXirType element=xr_xir_cell_element(types,(XrXirType)root->type);
    const XrXirTypeNode *array=xr_xir_type_node(types,element);
    if (element!=XR_XIR_STRING && (!array || array->kind!=XR_XIR_TYPE_ARRAY || array->element!=XR_XIR_U8))
        return XR_XIR_CTFE_NOT_SUPPORTED;
    XrXirValue snapshot={0};
    XrXirValueStatus status=xr_xir_cell_authorized_read(root,&state->authority,&snapshot);
    if (status!=XR_XIR_VALUE_OK) return ctfe_value_status(status);
    if (element==XR_XIR_STRING) {
        const char *bytes=NULL;size_t count=0;
        if (!xr_xir_string_view(&snapshot,&bytes,&count)) status=XR_XIR_VALUE_BAD_ARGUMENT;
        else if (count>(size_t)INT64_MAX) status=XR_XIR_VALUE_LIMIT;
        else *length=(int64_t)count;
    } else status=xr_xir_array_len(&snapshot,context->admission,length);
    xr_xir_value_drop(&snapshot);return ctfe_value_status(status);
}
typedef struct CtfeViewRange { int64_t start,end;bool to_end; } CtfeViewRange;
static XrXirCtfeStatus ctfe_view_create(CtfeViewAccessContext *context,
    const XrXirInstruction *op,const XrXirValue *root,CtfeViewRange range,XrXirValue *output) {
    int64_t start=range.start,end=range.end;
    CtfeOwnedState *state=&context->execution->owned;
    XrXirViewAccess access={context->admission,ctfe_view_authorize,context};
    bool reborrow=op->op==XR_XIR_VIEW_REBORROW;int64_t length=0;
    XrXirCtfeStatus status=XR_XIR_CTFE_OK;
    if (reborrow) status=ctfe_value_status(xr_xir_view_len(root,&access,&length));
    else status=ctfe_view_root_length(context,root,&length);
    if (context->failure!=XR_XIR_CTFE_OK) return context->failure;
    if (status!=XR_XIR_CTFE_OK) return status;
    if (range.to_end) end=length;
    if (start<0 || start>length || end<start || end>length)
        return ctfe_bounds_fault(state,(XrXirFaultDetail){XR_XIR_PANIC_BOUNDS,0,start<0 || start>length?start:end,length});
    CtfeViewBinding *binding=NULL;status=ctfe_view_reserve(context,&binding);
    if (status!=XR_XIR_CTFE_OK) return status;
    XrXirViewStorageSpec spec={state->authority,start,end-start,XR_XIR_TYPE_SLICE};
    XrXirValueStatus created;
    if (reborrow) created=xr_xir_view_reborrow(op->type,root,&spec,&access,output);
    else {
        const XrXirCellAuthority *parent=NULL;
        for (uint32_t i=0;i<context->frame->loan_count;++i) {
            const CtfeCellLoan *loan=&context->frame->loans[i];
            if (loan->value.type==root->type && loan->value.payload==root->payload) {
                if (!context->frame->loans_committed) {ctfe_view_binding_free(context->frame,binding);return XR_XIR_CTFE_BAD_ARTIFACT;}
                parent=&state->authority;break;
            }
        }
        created=xr_xir_view_new(op->type,root,parent,&spec,context->admission,output);
    }
    if (created==XR_XIR_VALUE_OK) {
        xr_xir_view_scope_pin_created(output,&spec.authority,&binding->pin);binding->type=op->type;
        binding->next=context->frame->views;context->frame->views=binding;
    } else ctfe_view_binding_free(context->frame,binding);
    return context->failure!=XR_XIR_CTFE_OK?context->failure:ctfe_value_status(created);
}
/* A call receives a distinct shared loan, authorized by the live caller leaf.
 * Neither an equal root nor a retained physical pin substitutes for that grant. */
static XrXirCtfeStatus ctfe_view_argument(CtfeCallExecution *execution,CtfeCallFrame *child,
    uint32_t parameter,const XrXirValue *source) {
    XrXirValueAdmission admission=ctfe_value_admission(&execution->owned);
    CtfeViewAccessContext context={execution,execution->top,&admission,XR_XIR_CTFE_OK};
    if (!ctfe_view_current(&context) || !child || child==execution->top ||
        child->execution!=execution || child->parent!=execution->top || !child->epoch ||
        child->epoch==execution->top->epoch || parameter>=child->run.function->parameter_count)
        return XR_XIR_CTFE_BAD_ARTIFACT;
    XrXirType type=child->run.function->parameters[parameter];
    if (!ctfe_slice_u8_type(child->run.module->types,type) || child->cleanup)
        return XR_XIR_CTFE_NOT_SUPPORTED;
    uint32_t offset=child->run.layout->offsets[parameter];
    if (!source || source->type!=(uint32_t)type || !xr_xir_value_argument(source,admission.arena,type) ||
        xr_xir_scalar_load(child->run.frame,offset)) return XR_XIR_CTFE_BAD_ARTIFACT;
    XrXirCtfeStatus status=ctfe_control_poll(execution->owned.control);
    if (status!=XR_XIR_CTFE_OK) return status;
    XrXirViewAccess access={&admission,ctfe_view_authorize,&context};int64_t length=0;
    status=ctfe_value_status(xr_xir_view_len(source,&access,&length));
    if (context.failure!=XR_XIR_CTFE_OK) return context.failure;
    if (status!=XR_XIR_CTFE_OK) return status;
    CtfeViewBinding *binding=NULL;status=ctfe_view_reserve(&context,&binding);
    if (status!=XR_XIR_CTFE_OK) return status;
    XrXirViewStorageSpec spec={{execution,child,child->epoch},0,length,XR_XIR_TYPE_SLICE};
    XrXirValue received={0};
    XrXirValueStatus copied=xr_xir_view_reborrow(type,source,&spec,&access,&received);
    if (copied==XR_XIR_VALUE_OK) {
        xr_xir_view_scope_pin_created(&received,&spec.authority,&binding->pin);binding->type=type;
        binding->next=child->views;child->views=binding;
        xr_xir_owned_slot_move(child->run.frame,offset,&received);
    } else ctfe_view_binding_free(child,binding);
    xr_xir_value_drop(&received);
    return context.failure!=XR_XIR_CTFE_OK?context.failure:ctfe_value_status(copied);
}
static XrXirCtfeStatus ctfe_view_cleanup(CtfeCallExecution *execution,const XrXirInstruction *op) {
    CtfeCallFrame *frame=execution->top;ScalarRun *run=&frame->run;
    if (op->args[0]>run->function->operand_count || op->args[1]>run->function->operand_count-op->args[0])
        return XR_XIR_CTFE_BAD_ARTIFACT;
    if (!op->args[1]) {ctfe_view_collect(frame);++frame->state.instruction;return XR_XIR_CTFE_OK;}
    const uint32_t *ids=run->function->operands+op->args[0];
    for (uint32_t i=0;i<op->args[1];++i) {
        XrXirCtfeStatus controlled=ctfe_control_poll(execution->owned.control);
        if (controlled!=XR_XIR_CTFE_OK) return controlled;
        XrXirType type=xr_xir_operand_type(run->function,ids[i]);
        if (!ctfe_slice_u8_type(run->module->types,type)) return XR_XIR_CTFE_NOT_SUPPORTED;
        XrXirValue value={type,0,xr_xir_scalar_load(run->frame,run->layout->offsets[ids[i]])};
        if (value.payload && !xr_xir_value_argument(&value,execution->owned.arena,type)) return XR_XIR_CTFE_BAD_ARTIFACT;
        for (uint32_t j=0;j<i;++j)
            if (run->layout->offsets[ids[i]]==run->layout->offsets[ids[j]]) return XR_XIR_CTFE_BAD_ARTIFACT;
    }
    /* All validation precedes the allocation-free owner-word retirement. */
    for (uint32_t i=0;i<op->args[1];++i) xr_xir_owned_slot_clear(run->frame,run->layout->offsets[ids[i]]);
    ctfe_view_collect(frame);++frame->state.instruction;return XR_XIR_CTFE_OK;
}
static XrXirCtfeStatus ctfe_view_step(CtfeCallExecution *execution) {
    CtfeCallFrame *frame=execution->top;ScalarRun *run=&frame->run;
    const XrXirInstruction *op=&run->function->instructions[frame->state.instruction];
    CtfeOwnedState *state=&execution->owned;
    XrXirCtfeStatus status=ctfe_control_poll(state->control);if (status!=XR_XIR_CTFE_OK) return status;
    XrXirValueAdmission admission=ctfe_value_admission(state);
    CtfeViewAccessContext context={execution,frame,&admission,XR_XIR_CTFE_OK};
    if (!ctfe_view_current(&context)) return XR_XIR_CTFE_BAD_ARTIFACT;
    if (op->op==XR_XIR_VIEW_CLEANUP) return ctfe_view_cleanup(execution,op);
    if (!frame->state.arguments) return XR_XIR_CTFE_BAD_ARTIFACT;
    bool group=xr_xir_op_uses_operand_table(op->op);
    const uint32_t *args=group?run->function->operands+op->args[0]:op->args;
    XrXirValue root=ctfe_nominal_operand(run,args[0]);XrXirValue *scratch=frame->state.arguments;
    scratch[0]=scratch[1]=(XrXirValue){0};
    XrXirViewAccess access={&admission,ctfe_view_authorize,&context};XrXirFaultDetail fault={0};
    if (op->op==XR_XIR_VIEW_NEW || op->op==XR_XIR_VIEW_REBORROW) {
        if (!ctfe_slice_u8_type(run->module->types,op->type)) return XR_XIR_CTFE_NOT_SUPPORTED;
        if (op->op==XR_XIR_VIEW_NEW) {
            status=ctfe_value_status(xr_xir_value_copy(&root,scratch));
            if (status!=XR_XIR_CTFE_OK) return status;
            root=*scratch;
        }
        int64_t start=xr_xir_scalar_load(run->frame,run->layout->offsets[args[1]]);
        int64_t end=xr_xir_scalar_load(run->frame,run->layout->offsets[args[2]]);
        status=ctfe_view_create(&context,op,&root,(CtfeViewRange){start,end,op->immediate!=0},scratch+1);
    } else if (!ctfe_slice_u8_type(run->module->types,(XrXirType)root.type)) status=XR_XIR_CTFE_NOT_SUPPORTED;
    else if (op->op==XR_XIR_VIEW_GET) {
        int64_t index=xr_xir_scalar_load(run->frame,run->layout->offsets[args[1]]);
        XrXirValueStatus got=xr_xir_view_get(&root,index,&access,scratch+1,&fault);
        status=got==XR_XIR_VALUE_BOUNDS?ctfe_bounds_fault(state,fault):ctfe_value_status(got);
    } else {
        int64_t length=0;status=ctfe_value_status(xr_xir_view_len(&root,&access,&length));
        if (status==XR_XIR_CTFE_OK && op->op==XR_XIR_VIEW_LEN) scratch[1]=(XrXirValue){XR_XIR_I64,0,length};
        else if (status==XR_XIR_CTFE_OK && op->op==XR_XIR_VIEW_END) {
            *scratch=root;xr_xir_scalar_store(run->frame,run->layout->offsets[args[0]],0);
        }
    }
    if (context.failure!=XR_XIR_CTFE_OK) status=context.failure;
    xr_xir_value_drop(scratch);
    if (status==XR_XIR_CTFE_OK && op->type!=XR_XIR_UNIT) {
        uint32_t destination=run->function->parameter_count+frame->state.instruction;
        if (xr_xir_type_is_owned(run->module->types,op->type))
            xr_xir_owned_slot_move(run->frame,run->layout->offsets[destination],scratch+1);
        else {xr_xir_scalar_store(run->frame,run->layout->offsets[destination],scratch[1].payload);scratch[1]=(XrXirValue){0};}
    }
    xr_xir_value_drop(scratch+1);ctfe_view_collect(frame);
    if (status==XR_XIR_CTFE_OK) ++frame->state.instruction;
    return status;
}
